// SPDX-License-Identifier: GPL-3.0-only

#include "video_decoder.hpp"
#include "core/runtime_journal.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace lunaone::apple
{
namespace
{
struct StartCode
{
    std::size_t offset;
    std::size_t length;
};

StartCode FindStartCode(std::span<const std::uint8_t> bytes, std::size_t from)
{
    for (std::size_t i = from; i + 2 < bytes.size(); ++i)
    {
        if (bytes[i] != 0 || bytes[i + 1] != 0)
            continue;
        if (bytes[i + 2] == 1)
            return {i, 3};
        if (bytes[i + 2] == 0 && i + 3 < bytes.size() && bytes[i + 3] == 1)
            return {i, 4};
    }
    return {bytes.size(), 0};
}

std::string FourCC(OSType code)
{
    std::string text(4, ' ');
    for (int i = 0; i < 4; ++i)
        text[i] = static_cast<char>((code >> (24 - 8 * i)) & 0xff);
    return text;
}

std::string Describe(CFTypeRef value)
{
    if (!value)
        return "none";
    if (CFGetTypeID(value) == CFBooleanGetTypeID())
        return CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? "yes" : "no";
    if (CFGetTypeID(value) != CFStringGetTypeID())
        return "?";
    char text[96];
    if (!CFStringGetCString(static_cast<CFStringRef>(value), text, sizeof(text), kCFStringEncodingUTF8))
        return "?";
    return text;
}

const char* ProfileName(std::uint8_t profile_idc, std::uint8_t constraints)
{
    switch (profile_idc)
    {
    case 66: return (constraints & 0x40) ? "constrained-baseline" : "baseline";
    case 77: return "main";
    case 100: return "high";
    default: return "other";
    }
}
} // namespace

VideoDecoder::VideoDecoder(std::function<void()> frame_ready, Mode mode, Acceleration acceleration)
    : frame_ready_(std::move(frame_ready)),
      flags_(mode == Mode::Asynchronous ? kVTDecodeFrame_EnableAsynchronousDecompression : 0),
      acceleration_(acceleration)
{
}

VideoDecoder::~VideoDecoder()
{
    Reset();
    if (latest_)
        CFRelease(latest_);
}

void VideoDecoder::Reset()
{
    if (session_)
    {
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        VTDecompressionSessionInvalidate(session_);
        CFRelease(std::exchange(session_, nullptr));
        pending_.store(0);
        restarts_.fetch_add(1, std::memory_order_relaxed);
    }
    if (format_)
        CFRelease(std::exchange(format_, nullptr));
    needs_idr_.store(true);
}

bool VideoDecoder::CreateSession()
{
    const std::uint8_t* sets[] = {sps_.data(), pps_.data()};
    const std::size_t sizes[] = {sps_.size(), pps_.size()};
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, sets, sizes, 4, &format_) != noErr)
        return false;

    const bool created = acceleration_ == Acceleration::Software
        ? CreateSession(false, false)
        : (acceleration_ == Acceleration::Hardware && CreateSession(true, true)) || CreateSession(true, false);
    if (!created)
    {
        Reset();
        return false;
    }
    picture_logged_.store(false);
    LogStream();
    return true;
}

bool VideoDecoder::CreateSession(bool hardware, bool require_hardware)
{
    auto spec = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
    if (!spec)
        return false;
    CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                         hardware ? kCFBooleanTrue : kCFBooleanFalse);
    if (require_hardware)
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);

    // 4:2:0 bi-planar, with the stream's own range listed first so that the
    // decoder hands over its samples without a range conversion. Metal-
    // compatible IOSurfaces reach the picture renderer without a copy.
    const auto signalled = CMFormatDescriptionGetExtension(format_, kCMFormatDescriptionExtension_FullRangeVideo);
    const bool full_range = signalled && CFGetTypeID(signalled) == CFBooleanGetTypeID() &&
                            CFBooleanGetValue(static_cast<CFBooleanRef>(signalled));
    const SInt32 video_range = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const SInt32 full_range_format = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef numbers[] = {
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, full_range ? &full_range_format : &video_range),
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, full_range ? &video_range : &full_range_format)};
    auto formats = CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void**>(numbers), 2, &kCFTypeArrayCallBacks);
    CFRelease(numbers[0]);
    CFRelease(numbers[1]);
    auto surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferMetalCompatibilityKey,
                          kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {formats, kCFBooleanTrue, surface};
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks);
    CFRelease(formats);
    CFRelease(surface);

    const VTDecompressionOutputCallbackRecord callback {Decoded, this};
    VTDecompressionSessionRef session = nullptr;
    const auto status = VTDecompressionSessionCreate(
        kCFAllocatorDefault, format_, spec, attributes, &callback, &session);
    CFRelease(spec);
    CFRelease(attributes);
    if (status != noErr || !session)
        return false;
    session_ = session;

    VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    hardware_.store(false);
    CFTypeRef used = nullptr;
    if (VTSessionCopyProperty(session_, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                              kCFAllocatorDefault, &used) == noErr && used)
    {
        hardware_.store(CFEqual(used, kCFBooleanTrue));
        CFRelease(used);
    }
    return true;
}

void VideoDecoder::LogStream() const
{
    const auto size = CMVideoFormatDescriptionGetDimensions(format_);
    const std::uint8_t profile = sps_.size() > 3 ? sps_[1] : 0;
    const std::uint8_t constraints = sps_.size() > 3 ? sps_[2] : 0;
    const std::uint8_t level = sps_.size() > 3 ? sps_[3] : 0;
    lunanx::LogRuntimeEvent("apple", "video_stream",
        "profile=" + std::string(ProfileName(profile, constraints)) + " profile_idc=" + std::to_string(profile) +
        " level=" + std::to_string(level / 10) + "." + std::to_string(level % 10) +
        " size=" + std::to_string(size.width) + "x" + std::to_string(size.height) +
        " full_range=" + Describe(CMFormatDescriptionGetExtension(format_, kCMFormatDescriptionExtension_FullRangeVideo)) +
        " matrix=" + Describe(CMFormatDescriptionGetExtension(format_, kCMFormatDescriptionExtension_YCbCrMatrix)) +
        " primaries=" + Describe(CMFormatDescriptionGetExtension(format_, kCMFormatDescriptionExtension_ColorPrimaries)) +
        " transfer=" + Describe(CMFormatDescriptionGetExtension(format_, kCMFormatDescriptionExtension_TransferFunction)) +
        " decoder=" + (hardware_.load() ? "hardware" : "software"));
}

void VideoDecoder::LogPicture(CVImageBufferRef image)
{
    const auto matrix = CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr);
    lunanx::LogRuntimeEvent("apple", "video_picture",
        "format=" + FourCC(CVPixelBufferGetPixelFormatType(image)) +
        " size=" + std::to_string(CVPixelBufferGetWidth(image)) + "x" + std::to_string(CVPixelBufferGetHeight(image)) +
        " matrix=" + Describe(matrix) + " iosurface=" + (CVPixelBufferGetIOSurface(image) ? "yes" : "no"));
    if (matrix)
        CFRelease(matrix);
}

bool VideoDecoder::Submit(std::span<const std::uint8_t> annex_b, std::uint32_t timestamp)
{
    std::vector<std::uint8_t> avcc;
    avcc.reserve(annex_b.size() + 16);
    bool idr = false;
    bool slice = false;
    bool format_changed = false;
    for (auto code = FindStartCode(annex_b, 0); code.length != 0;)
    {
        const auto beginning = code.offset + code.length;
        code = FindStartCode(annex_b, beginning);
        if (code.offset == beginning)
            continue;
        const auto nal = annex_b.subspan(beginning, code.offset - beginning);
        const auto type = nal.front() & 0x1f;
        if (type == 7 || type == 8)
        {
            auto& set = type == 7 ? sps_ : pps_;
            if (!std::ranges::equal(set, nal))
            {
                set.assign(nal.begin(), nal.end());
                format_changed = true;
            }
            continue;
        }
        if (type == 9)
            continue;
        slice = slice || (type >= 1 && type <= 5);
        idr = idr || type == 5;
        const auto size = static_cast<std::uint32_t>(nal.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            avcc.push_back(static_cast<std::uint8_t>(size >> shift));
        avcc.insert(avcc.end(), nal.begin(), nal.end());
    }

    if (format_changed && session_)
        Reset();
    if (!slice)
        return true;
    const bool needs_keyframe = needs_idr_.load() || pending_.load() >= 5;
    if ((needs_keyframe && !idr) || sps_.empty() || pps_.empty())
    {
        needs_idr_.store(true);
        awaiting_keyframe_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (needs_keyframe && session_)
        Reset();
    if (!session_)
    {
        if (!CreateSession())
            return false;
        first_timestamp_ = timestamp;
    }

    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(), kCFAllocatorDefault,
                                          nullptr, 0, avcc.size(), kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
        return false;
    const std::size_t size = avcc.size();
    const CMSampleTimingInfo timing {kCMTimeInvalid,
        CMTimeMake(static_cast<std::int32_t>(timestamp - first_timestamp_), 90'000), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    auto status = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, size);
    if (status == noErr)
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 1, &timing, 1, &size, &sample);
    CFRelease(block);
    if (status != noErr)
        return false;

    needs_idr_.store(false);
    pending_.fetch_add(1);
    status = VTDecompressionSessionDecodeFrame(session_, sample, flags_, nullptr, nullptr);
    CFRelease(sample);
    if (status == noErr)
        return !needs_idr_.load();
    pending_.fetch_sub(1);
    failures_.fetch_add(1, std::memory_order_relaxed);
    last_error_.store(status);
    Reset();
    return false;
}

void VideoDecoder::Decoded(void* context, void*, OSStatus status, VTDecodeInfoFlags flags,
                           CVImageBufferRef image, CMTime, CMTime)
{
    auto& decoder = *static_cast<VideoDecoder*>(context);
    decoder.pending_.fetch_sub(1);
    if (status == noErr && !image && (flags & kVTDecodeInfo_FrameDropped))
    {
        decoder.dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (status != noErr || !image)
    {
        decoder.failures_.fetch_add(1, std::memory_order_relaxed);
        decoder.last_error_.store(status);
        decoder.needs_idr_.store(true);
        return;
    }
    if (!decoder.picture_logged_.exchange(true))
        LogPicture(image);
    CFRetain(image);
    bool notify;
    {
        std::lock_guard lock(decoder.mutex_);
        notify = !decoder.latest_;
        if (decoder.latest_)
            CFRelease(decoder.latest_);
        decoder.latest_ = image;
    }
    decoder.decoded_.fetch_add(1);
    if (notify && decoder.frame_ready_)
        decoder.frame_ready_();
}

VideoDecoder::Counters VideoDecoder::Stats() const
{
    return {awaiting_keyframe_.load(), failures_.load(), dropped_.load(), restarts_.load(), last_error_.load()};
}

CVImageBufferRef VideoDecoder::TakeFrame()
{
    std::lock_guard lock(mutex_);
    return std::exchange(latest_, nullptr);
}

} // namespace lunaone::apple
