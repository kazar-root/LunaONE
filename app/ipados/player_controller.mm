// SPDX-License-Identifier: GPL-3.0-only

#include "player_controller.hpp"
#include "audio_output.hpp"
#include "core/runtime_journal.hpp"
#include "playback_support.hpp"
#include "video_decoder.hpp"
#include "stream/DecodeQueuePolicy.hpp"
#include "stream/performance_overlay.hpp"
#include "stream/launch_status.hpp"
#include "core/playback_preferences.hpp"
#include "webrtc/luna_stream_session.hpp"
#import "watch_mirror.hpp"

#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using lunanx::LunaStreamSession;

// The controller in hand and the buttons held on a paired Apple Watch, sent
// together. The lock also keeps the overlay to one caller at a time.
struct PadMix
{
    std::mutex lock;
    lunanx::input::GamepadState physical;
    std::uint16_t watch = 0;
};

struct Playback
{
    struct VideoUnit
    {
        std::vector<std::uint8_t> data;
        std::uint32_t timestamp;
        Clock::time_point arrival;
    };

    lunaone::apple::VideoDecoder video;
    std::unique_ptr<lunaone::apple::AudioOutput> audio = std::make_unique<lunaone::apple::AudioOutput>();
    LunaStreamSession stream;
    std::mutex videoMutex;
    std::condition_variable videoReady;
    std::deque<VideoUnit> pendingVideo;
    const std::size_t burstLimit = lunanx::video::MaximumBurstAccessUnits(lunanx::LoadPlaybackPreferences().fps);
    std::size_t pendingVideoBytes = 0;
    bool discontinuity = false;
    bool videoStopping = false;
    std::atomic<bool> needsKeyframe {false};
    std::atomic<std::uint64_t> droppedVideo {0};
    std::thread decodeWorker;

    Playback(const lunanx::luna::LunaClient& client, lunanx::luna::Game game,
             std::function<void()> frameReady, std::function<void(const std::string&)> status)
        : video(std::move(frameReady), lunaone::apple::VideoDecoder::Mode::Synchronous), stream(client, std::move(game), {
            [this](auto bytes, auto timestamp) { return queueVideo(bytes, timestamp); },
            [this](auto bytes, auto sequence) { audio->Submit(bytes, sequence); },
            std::move(status)}), decodeWorker([this] { decodeVideo(); })
    {
    }

    ~Playback() { stop(); }

    void stop(bool endRemoteSession = false)
    {
        stream.Stop(endRemoteSession);
        if (decodeWorker.joinable())
        {
            {
                std::lock_guard lock(videoMutex);
                videoStopping = true;
            }
            videoReady.notify_one();
            decodeWorker.join();
        }
    }

    bool queueVideo(std::span<const std::uint8_t> bytes, std::uint32_t timestamp)
    {
        const auto now = Clock::now();
        if (bytes.size() > lunanx::video::MaximumBurstQueueBytes) return false;
        VideoUnit unit {{bytes.begin(), bytes.end()}, timestamp, now};
        {
            std::lock_guard lock(videoMutex);
            // Absorb a short decode interruption without breaking H.264 references.
            if (pendingVideo.size() >= burstLimit ||
                unit.data.size() > lunanx::video::MaximumBurstQueueBytes - pendingVideoBytes ||
                (!pendingVideo.empty() && now - pendingVideo.front().arrival >
                    std::chrono::milliseconds(lunanx::video::MaximumBurstQueueDelayMs)))
            {
                droppedVideo += pendingVideo.size();
                pendingVideo.clear();
                pendingVideoBytes = 0;
                discontinuity = true;
                needsKeyframe.store(true);
            }
            pendingVideoBytes += unit.data.size();
            pendingVideo.push_back(std::move(unit));
        }
        videoReady.notify_one();
        return !needsKeyframe.exchange(false);
    }

    void decodeVideo()
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        for (;;)
        {
            std::unique_lock lock(videoMutex);
            videoReady.wait(lock, [this] { return videoStopping || !pendingVideo.empty(); });
            if (videoStopping)
                return;
            auto unit = std::move(pendingVideo.front());
            pendingVideo.pop_front();
            pendingVideoBytes -= unit.data.size();
            if (std::exchange(discontinuity, false))
                video.RequireKeyframe();
            lock.unlock();
            @autoreleasepool
            {
                try
                {
                    if (!video.Submit(unit.data, unit.timestamp))
                        needsKeyframe.store(true);
                }
                catch (const std::exception&) { needsKeyframe.store(true); }
            }
        }
    }
};

} // namespace

@implementation LunaPlayerController
{
    std::shared_ptr<const lunanx::luna::LunaClient> _client;
    lunanx::luna::Game _game;
    std::shared_ptr<Playback> _playback;
    AVSampleBufferDisplayLayer* _videoLayer;
    CADisplayLink* _displayLink;
    UIStackView* _statusPanel;
    UIImageView* _launchLogo;
    NSArray<UIView*>* _squares;
    int _launchStage, _launchPulse;
    UILabel* _status;
    UILabel* _hud;
    // Shared with the controller queue, which updates it on every change.
    std::shared_ptr<lunanx::PerformanceOverlay> _overlay;
    std::shared_ptr<std::atomic<bool>> _exitRequested;
    std::unique_ptr<lunaone::apple::GamepadFeed> _gamepads;
    std::unique_ptr<lunaone::apple::NetworkStatus> _network;
    Clock::time_point _nextHudUpdate;
    Clock::time_point _started;
    Clock::time_point _firstFrame;
    Clock::time_point _lastFrame;
    std::chrono::milliseconds _maxFrameGap;
    std::uint64_t _longFrameGaps;
    std::uint64_t _presented;
    int _decodedHeight;
    std::uint64_t _rejected;
    BOOL _controllerSeen;
    BOOL _startedOnce;
    BOOL _stopping;
    BOOL _dismissOnStop;
    BOOL _endRemoteOnStop;
}

- (instancetype)initWithClient:(std::shared_ptr<const lunanx::luna::LunaClient>)client
                          game:(lunanx::luna::Game)game
{
    if ((self = [super initWithNibName:nil bundle:nil]))
    {
        _client = std::move(client);
        _game = std::move(game);
        _maxFrameGap = std::chrono::milliseconds::zero();
    }
    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    auto surface = [UIView new];
    surface.userInteractionEnabled = NO;
    surface.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    surface.frame = self.view.bounds;
    _videoLayer = [AVSampleBufferDisplayLayer new];
    _videoLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    _videoLayer.backgroundColor = UIColor.blackColor.CGColor;
    [surface.layer addSublayer:_videoLayer];
    [self.view addSubview:surface];


    _hud = [UILabel new];
    _hud.textAlignment = NSTextAlignmentRight;
    _hud.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightSemibold];
    _hud.textColor = UIColor.whiteColor;
    _hud.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.68];
    _hud.layer.cornerRadius = 6;
    _hud.layer.masksToBounds = YES;
    _hud.translatesAutoresizingMaskIntoConstraints = NO;
    _hud.hidden = YES;
    [self.view addSubview:_hud];

    _launchLogo = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"LunaONE.png"]];
    _launchLogo.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_launchLogo];
    auto cover = [UIImageView new];
    cover.contentMode = UIViewContentModeScaleAspectFill;
    cover.clipsToBounds = YES;
    cover.layer.cornerRadius = 12;
    cover.backgroundColor = [UIColor colorWithRed:.12 green:.09 blue:.21 alpha:1];
    const CGFloat coverHeight = self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassCompact ? 124 : 188;
    [cover.heightAnchor constraintEqualToConstant:coverHeight].active = YES;
    [cover.widthAnchor constraintEqualToConstant:coverHeight * .75].active = YES;
    lunaone::apple::FetchLaunchCover(_game.image_portrait.empty() ? _game.image_landscape : _game.image_portrait,
        ^(NSData* bytes) { cover.image = [UIImage imageWithData:bytes]; });
    auto title = [UILabel new];
    title.text = [NSString stringWithUTF8String:_game.title.c_str()] ?: @"Amazon Luna";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    title.adjustsFontForContentSizeCategory = YES;
    title.numberOfLines = 0;
    title.textAlignment = NSTextAlignmentCenter;
    _status = [UILabel new];
    _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _status.adjustsFontForContentSizeCategory = YES;
    _status.textAlignment = NSTextAlignmentCenter;
    _status.numberOfLines = 0;
    _status.text = @"Préparation du flux…";
    _status.textColor = UIColor.secondaryLabelColor;
    auto progress = [UIStackView new];
    progress.axis = UILayoutConstraintAxisHorizontal;
    progress.spacing = 9;
    auto squares = [NSMutableArray<UIView*> arrayWithCapacity:3];
    for (int i = 0; i < 3; ++i)
    {
        auto square = [UIView new];
        square.backgroundColor = i == 0 ? UIColor.whiteColor : [UIColor colorWithWhite:1 alpha:.25];
        square.layer.cornerRadius = 2;
        [square.widthAnchor constraintEqualToConstant:10].active = YES;
        [square.heightAnchor constraintEqualToConstant:10].active = YES;
        [progress addArrangedSubview:square];
        [squares addObject:square];
    }
    _launchStage = 1;
    _squares = squares;
    _statusPanel = [[UIStackView alloc] initWithArrangedSubviews:@[cover, title, _status, progress]];
    _statusPanel.alignment = UIStackViewAlignmentCenter;
    _statusPanel.axis = UILayoutConstraintAxisVertical;
    _statusPanel.spacing = 14;
    _statusPanel.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_statusPanel];
    auto safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_launchLogo.topAnchor constraintEqualToAnchor:safe.topAnchor constant:15],
        [_launchLogo.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [_launchLogo.widthAnchor constraintEqualToConstant:43],
        [_launchLogo.heightAnchor constraintEqualToConstant:43],
        [_hud.topAnchor constraintEqualToAnchor:safe.topAnchor constant:18],
        [_hud.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_statusPanel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [_statusPanel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [_statusPanel.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:22],
        [_statusPanel.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-22],
    ]];
    auto exit = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(closePlayer)];
    exit.direction = UISwipeGestureRecognizerDirectionDown;
    exit.numberOfTouchesRequired = 2;
    [surface.superview addGestureRecognizer:exit];
    [self addKeyCommand:[UIKeyCommand keyCommandWithInput:UIKeyInputEscape modifierFlags:0 action:@selector(closePlayer)]];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _videoLayer.frame = self.view.bounds;
    [CATransaction commit];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    if (_startedOnce)
        return;
    _startedOnce = YES;
    _overlay = std::make_shared<lunanx::PerformanceOverlay>(lunanx::LoadPlaybackPreferences().diagnostics);
    _exitRequested = std::make_shared<std::atomic<bool>>(false);
    [self becomeFirstResponder];
    auto audio = AVAudioSession.sharedInstance;
    NSError* error = nil;
    if (![audio setCategory:AVAudioSessionCategoryPlayback error:&error] || ![audio setActive:YES error:&error])
    {
        [self stopPlaybackWithMessage:error.localizedDescription dismiss:NO];
        return;
    }
    auto notifications = NSNotificationCenter.defaultCenter;
    [notifications addObserver:self selector:@selector(backgrounded:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [notifications addObserver:self selector:@selector(audioInterrupted:) name:AVAudioSessionInterruptionNotification object:audio];
    __weak LunaPlayerController* weakSelf = self;
    NSString* gameTitle = [NSString stringWithUTF8String:_game.title.c_str()] ?: @"Amazon Luna";
    try
    {
        _playback = std::make_shared<Playback>(*_client, std::move(_game), [weakSelf] {
            dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf presentLatestFrame]; });
        }, [weakSelf](const std::string& status) {
            @autoreleasepool
            {
                const int stage = lunanx::LaunchStage(status);
                auto text = lunanx::LaunchStatusText(status, stage);
                NSString* message = [NSString stringWithUTF8String:text.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf showSessionStatus:message stage:stage]; });
            }
        });
        _started = Clock::now();
        _network = std::make_unique<lunaone::apple::NetworkStatus>();
        _playback->stream.Start();
        // Each controller change goes to the session at once instead of
        // waiting up to 16.7 ms for the next display refresh.
        const std::weak_ptr<Playback> weakPlayback = _playback;
        const auto mix = std::make_shared<PadMix>();
        _gamepads = std::make_unique<lunaone::apple::GamepadFeed>(
            [weakPlayback, overlay = _overlay, exitRequested = _exitRequested, mix](const lunanx::input::GamepadState& pad) {
                if (lunanx::input::ExitStreamChord(pad))
                {
                    exitRequested->store(true);
                    return;
                }
                std::lock_guard lock(mix->lock);
                mix->physical = pad;
                auto merged = pad;
                merged.buttons |= mix->watch;
                if (const auto playback = weakPlayback.lock())
                    playback->stream.SetGamepad(overlay->Update(merged));
            });
        // A paired Apple Watch shows the picture and adds its buttons.
        [LunaWatchMirror.shared beginGame:gameTitle buttons:[weakPlayback, overlay = _overlay, mix](std::uint16_t watch) {
            std::lock_guard lock(mix->lock);
            mix->watch = watch;
            auto merged = mix->physical;
            merged.buttons |= watch;
            if (const auto playback = weakPlayback.lock())
                playback->stream.SetGamepad(overlay->Update(merged));
        }];
        _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(updatePlayback)];
        _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        UIApplication.sharedApplication.idleTimerDisabled = YES;
    }
    catch (const std::exception& ex)
    {
        [self stopPlaybackWithMessage:[NSString stringWithUTF8String:ex.what()] dismiss:NO];
    }
}

- (void)showSessionStatus:(NSString*)message stage:(int)stage
{
    if (!_playback || _presented || stage < _launchStage) return;
    _launchStage = stage;
    _status.text = message;
}

- (void)updatePlayback
{
    if (!_playback)
        return;
    if (_playback->stream.Finished())
    {
        const auto error = _playback->stream.Error();
        NSString* message = error.empty() ? @"La session est terminée." : [NSString stringWithUTF8String:error.c_str()];
        [self stopPlaybackWithMessage:message dismiss:NO];
        return;
    }
    auto controller = lunaone::apple::ActiveGamepad();
    _controllerSeen = _controllerSeen || controller != nil;
    if (_gamepads)
        _gamepads->Follow(controller);
    if (_exitRequested->load())
    {
        [self closePlayer];
        return;
    }
    _overlay->Sample({_playback->stream.Stats().received_bytes, _presented});
    const auto now = Clock::now();
    if (!_presented && !UIAccessibilityIsReduceMotionEnabled())
    {
        const auto next = lunanx::LaunchPulseIndex(now - _started);
        if (next != _launchPulse)
        {
            _launchPulse = next;
            for (NSInteger i = 0; i < 3; ++i)
                _squares[i].backgroundColor = i == next ? UIColor.whiteColor : [UIColor colorWithWhite:1 alpha:.25];
        }
    }
    if (_overlay->Visible() && _presented && (_hud.hidden || now >= _nextHudUpdate))
    {
        const auto line = _overlay->Format(_network ? _network->Label() : "", lunaone::apple::SystemBatteryPercent(), _decodedHeight);
        _hud.text = [NSString stringWithFormat:@"  %@  ", [NSString stringWithUTF8String:line.c_str()]];
        _nextHudUpdate = now + std::chrono::seconds(1);
    }
    _hud.hidden = !_overlay->Visible() || !_presented;
}

- (void)presentLatestFrame
{
    if (!_playback)
        return;
    if (auto image = _playback->video.TakeFrame())
    {
        if (!lunaone::apple::Present(_videoLayer, image))
            ++_rejected;
        else
        {
            [LunaWatchMirror.shared offerFrame:image];
            _decodedHeight = static_cast<int>(CVPixelBufferGetHeight(image));
            const auto now = Clock::now();
            if (_presented++ == 0)
            {
                _firstFrame = now;
                _statusPanel.hidden = _launchLogo.hidden = YES;
            }
            else
            {
                const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastFrame);
                _maxFrameGap = std::max(_maxFrameGap, gap);
                _longFrameGaps += gap > std::chrono::milliseconds(50);
            }
            _lastFrame = now;
        }
        CFRelease(image);
    }
}

- (BOOL)canBecomeFirstResponder { return YES; }

- (void)closePlayer
{
    _endRemoteOnStop = YES;
    [self stopPlaybackWithMessage:nil dismiss:YES];
}

- (void)backgrounded:(NSNotification*)notification
{
    [self stopPlaybackWithMessage:nil dismiss:YES];
}

- (void)audioInterrupted:(NSNotification*)notification
{
    if ([notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan)
        [self stopPlaybackWithMessage:@"La sortie audio a été interrompue." dismiss:NO];
}

- (void)stopPlaybackWithMessage:(NSString*)message dismiss:(BOOL)dismiss
{
    _dismissOnStop = _dismissOnStop || dismiss;
    if (_stopping)
        return;
    [_displayLink invalidate];
    _displayLink = nil;
    // Stops controller delivery and waits for a handler in progress, so the
    // controller queue never ends up owning the playback.
    _gamepads.reset();
    [LunaWatchMirror.shared endGame];
    [NSNotificationCenter.defaultCenter removeObserver:self];
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    _statusPanel.hidden = _launchLogo.hidden = NO;
    const auto playback = std::exchange(_playback, nullptr);
    if (!playback)
    {
        [AVAudioSession.sharedInstance setActive:NO error:nil];
        _status.text = message;
        if (_dismissOnStop)
            [self dismissViewControllerAnimated:YES completion:nil];
        return;
    }

    _stopping = YES;
    _status.text = message ?: @"Fermeture de la session…";
    const auto displayed = _presented;
    const auto rejected = _rejected;
    const auto maxFrameGap = _maxFrameGap.count();
    const auto longFrameGaps = _longFrameGaps;
    const auto lastPictureIdle = displayed ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - _lastFrame).count() : 0;
    const bool controllerSeen = _controllerSeen;
    const bool endRemoteSession = _endRemoteOnStop || (!displayed && message.length > 0);
    const auto duration = displayed ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - _firstFrame).count() : 0;
    const auto firstPicture = displayed ? std::chrono::duration_cast<std::chrono::milliseconds>(_firstFrame - _started).count() : 0;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool
        {
            playback->stop(endRemoteSession);
            const auto stats = playback->stream.Stats();
            const auto decode = playback->video.Stats();
            const std::string summary = "pictures=" + std::to_string(displayed) +
                " renderer_rejected=" + std::to_string(rejected) +
                " duration_ms=" + std::to_string(duration) +
                " first_picture_ms=" + std::to_string(firstPicture) +
                " max_frame_gap_ms=" + std::to_string(maxFrameGap) +
                " frame_gaps_over_50ms=" + std::to_string(longFrameGaps) +
                " last_picture_idle_ms=" + std::to_string(lastPictureIdle) +
                " decoded=" + std::to_string(playback->video.DecodedFrames()) +
                " decode_queue_dropped=" + std::to_string(playback->droppedVideo.load()) +
                " awaiting_keyframe=" + std::to_string(decode.awaiting_keyframe) +
                " decoder_failures=" + std::to_string(decode.failures) +
                " decoder_dropped=" + std::to_string(decode.dropped) +
                " decoder_restarts=" + std::to_string(decode.restarts) +
                " decoder_last_error=" + std::to_string(decode.last_error) +
                " hardware=" + (playback->video.HardwareActive() ? "yes" : "no") +
                " video_units=" + std::to_string(stats.video_units) +
                " video_units_dropped=" + std::to_string(stats.video_units_dropped) +
                " video_rtp_packets=" + std::to_string(stats.video_rtp_packets) +
                " video_ssrc_mismatches=" + std::to_string(stats.video_ssrc_mismatches) +
                " srtp_decrypt_failures=" + std::to_string(stats.srtp_decrypt_failures) +
                " audio_packets=" + std::to_string(stats.audio_packets) +
                " audio_non_silent_packets=" + std::to_string(playback->audio->NonSilentPackets()) +
                " audio_buffers=" + std::to_string(playback->audio->PlayedBuffers()) +
                " audio_underruns=" + std::to_string(playback->audio->UnderrunBuffers()) +
                " audio_dropped=" + std::to_string(playback->audio->DroppedPackets()) +
                " controller=" + (controllerSeen ? "yes" : "no") +
                " input_changes=" + std::to_string(stats.gamepad_changes) +
                " rtt_ms=" + std::to_string(stats.transport_rtt_ms);
            playback->audio.reset();
            NSLog(@"LunaONE iPadOS stream ended: %s", summary.c_str());
            lunanx::LogRuntimeEvent("ipados", "stream_ended", summary);
            [AVAudioSession.sharedInstance setActive:NO
                withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_stopping = NO;
                if (self->_dismissOnStop)
                    [self dismissViewControllerAnimated:YES completion:nil];
                else
                    self->_status.text = message ?: @"La session est terminée.";
            });
        }
    });
}
@end
