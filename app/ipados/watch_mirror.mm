// SPDX-License-Identifier: GPL-3.0-only

#import "watch_mirror.hpp"

#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#import <WatchConnectivity/WatchConnectivity.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace
{
// Eight pictures a second at most, 320 pixels wide: a 45 mm watch shows 396.
constexpr CFTimeInterval kFrameInterval = 1.0 / 8;
constexpr CGFloat kWidth = 320;
// D-pad, Start, Back, A, B, X and Y: the only buttons the watch may press.
constexpr std::uint16_t kWatchButtons = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 |
                                        0x1000 | 0x2000 | 0x4000 | 0x8000;
}

@interface LunaWatchMirror () <WCSessionDelegate>
@end

@implementation LunaWatchMirror
{
    dispatch_queue_t _queue;
    CIContext* _context;
    std::atomic<bool> _playing;
    std::atomic<bool> _inFlight;
    std::atomic<CFTimeInterval> _lastSent;
    std::mutex _lock;
    std::function<void(std::uint16_t)> _buttons;
}

+ (instancetype)shared
{
    static LunaWatchMirror* mirror;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mirror = [LunaWatchMirror new]; });
    return mirror;
}

- (instancetype)init
{
    if ((self = [super init]))
    {
        _queue = dispatch_queue_create("app.lunaone.watch-mirror", DISPATCH_QUEUE_SERIAL);
        _context = [CIContext contextWithOptions:nil];
        _playing = false;
        _inFlight = false;
        _lastSent = 0;
    }
    return self;
}

- (void)activate
{
    if (!WCSession.isSupported)
        return;
    WCSession.defaultSession.delegate = self;
    [WCSession.defaultSession activateSession];
}

- (BOOL)watchReady
{
    if (!WCSession.isSupported)
        return NO;
    WCSession* session = WCSession.defaultSession;
    return session.activationState == WCSessionActivationStateActivated && session.isPaired &&
           session.isWatchAppInstalled && session.isReachable;
}

- (void)beginGame:(NSString*)title buttons:(std::function<void(std::uint16_t)>)buttons
{
    {
        std::lock_guard lock(_lock);
        _buttons = std::move(buttons);
    }
    _inFlight = false;
    _playing = true;
    if ([self watchReady])
        [WCSession.defaultSession sendMessage:@{@"title": title ?: @""} replyHandler:nil errorHandler:nil];
}

- (void)endGame
{
    _playing = false;
    {
        std::lock_guard lock(_lock);
        _buttons = nullptr;
    }
    if ([self watchReady])
        [WCSession.defaultSession sendMessage:@{@"state": @"idle"} replyHandler:nil errorHandler:nil];
}

- (void)offerFrame:(CVPixelBufferRef)image
{
    const CFTimeInterval now = CACurrentMediaTime();
    if (!image || !_playing || _inFlight || now - _lastSent < kFrameInterval || ![self watchReady])
        return;
    _inFlight = true;
    _lastSent = now;
    CVPixelBufferRetain(image);
    dispatch_async(_queue, ^{
        NSData* jpeg = nil;
        @autoreleasepool
        {
            CIImage* picture = [CIImage imageWithCVPixelBuffer:image];
            const CGFloat scale = kWidth / MAX(picture.extent.size.width, 1.0);
            picture = [picture imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)];
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            jpeg = [self->_context JPEGRepresentationOfImage:picture colorSpace:space
                                                     options:@{(id)kCGImageDestinationLossyCompressionQuality: @0.5}];
            CGColorSpaceRelease(space);
        }
        CVPixelBufferRelease(image);
        if (!jpeg || !self->_playing)
        {
            self->_inFlight = false;
            return;
        }
        // The watch answers once the picture is on screen: the next one leaves then.
        [WCSession.defaultSession sendMessageData:jpeg
            replyHandler:^(NSData*) { self->_inFlight = false; }
            errorHandler:^(NSError*) { self->_inFlight = false; }];
    });
}

#pragma mark - WCSessionDelegate

- (void)session:(WCSession*)session activationDidCompleteWithState:(WCSessionActivationState)state error:(NSError*)error
{
}

- (void)sessionDidBecomeInactive:(WCSession*)session
{
}

- (void)sessionDidDeactivate:(WCSession*)session
{
    // The user paired another watch: follow it.
    [session activateSession];
}

// The watch sends the Luna button mask it holds: { "b": mask }.
- (void)session:(WCSession*)session didReceiveMessage:(NSDictionary<NSString*, id>*)message
{
    NSNumber* mask = message[@"b"];
    if (![mask isKindOfClass:NSNumber.class])
        return;
    std::function<void(std::uint16_t)> buttons;
    {
        std::lock_guard lock(_lock);
        buttons = _buttons;
    }
    if (buttons)
        buttons(static_cast<std::uint16_t>(mask.unsignedIntValue & kWatchButtons));
}
@end
