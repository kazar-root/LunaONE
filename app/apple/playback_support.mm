// SPDX-License-Identifier: GPL-3.0-only

#include "playback_support.hpp"
#include "ui/image_url_policy.hpp"

#include <cmath>

#if TARGET_OS_IOS
#import <CoreTelephony/CTTelephonyNetworkInfo.h>
#import <UIKit/UIKit.h>
#else
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/ps/IOPSKeys.h>
#endif

namespace lunaone::apple
{
using namespace lunanx::input;

void FetchLaunchCover(const std::string& address, void (^completion)(NSData*))
{
    const auto resized = lunanx::ResizeAmazonImageUrl(address, 480);
    auto url = [NSURL URLWithString:[NSString stringWithUTF8String:resized.c_str()]];
    if (![url.scheme isEqualToString:@"https"] ||
        !([url.host hasSuffix:@".media-amazon.com"] || [url.host hasSuffix:@".images-amazon.com"]))
        return;
    static NSURLSession* assets = [] {
        auto settings = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        settings.HTTPShouldSetCookies = NO;
        settings.timeoutIntervalForRequest = 6;
        return [NSURLSession sessionWithConfiguration:settings];
    }();
    [[assets dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        if (error || data.length == 0 || data.length > 6 * 1024 * 1024 ||
            [(NSHTTPURLResponse*)response statusCode] != 200) return;
        dispatch_async(dispatch_get_main_queue(), ^{ completion(data); });
    }] resume];
}

NetworkStatus::NetworkStatus()
    : monitor_(nw_path_monitor_create()), transport_(std::make_shared<std::atomic_int>(0))
{
    const auto transport = transport_;
    nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
        transport->store(nw_path_get_status(path) != nw_path_status_satisfied ? 0 :
            nw_path_uses_interface_type(path, nw_interface_type_wired) ? 1 :
            nw_path_uses_interface_type(path, nw_interface_type_wifi) ? 2 :
            nw_path_uses_interface_type(path, nw_interface_type_cellular) ? 3 : 0);
    });
    nw_path_monitor_set_queue(monitor_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    nw_path_monitor_start(monitor_);
}

NetworkStatus::~NetworkStatus() { nw_path_monitor_cancel(monitor_); }

std::string NetworkStatus::Label() const
{
    switch (transport_->load())
    {
    case 1: return "LAN";
    case 2: return "Wi-Fi";
    case 3:
#if TARGET_OS_IOS
        {
            static CTTelephonyNetworkInfo* const phone = [CTTelephonyNetworkInfo new];
            NSString* service = phone.dataServiceIdentifier;
            NSString* technology = service ? phone.serviceCurrentRadioAccessTechnology[service] : nil;
            if ([technology isEqualToString:CTRadioAccessTechnologyNR] ||
                [technology isEqualToString:CTRadioAccessTechnologyNRNSA])
                return "5G";
        }
#endif
        return "Mobile";
    default: return {};
    }
}

int SystemBatteryPercent()
{
#if TARGET_OS_IOS
    auto device = UIDevice.currentDevice;
    device.batteryMonitoringEnabled = YES;
    return device.batteryLevel < 0 ? -1 : static_cast<int>(std::lround(device.batteryLevel * 100));
#else
    auto information = IOPSCopyPowerSourcesInfo();
    if (!information) return -1;
    auto list = IOPSCopyPowerSourcesList(information);
    int battery = -1;
    for (CFIndex index = 0; list && index < CFArrayGetCount(list); ++index)
    {
        NSDictionary* source = (__bridge NSDictionary*)IOPSGetPowerSourceDescription(information, CFArrayGetValueAtIndex(list, index));
        if (![source[@kIOPSTypeKey] isEqual:@kIOPSInternalBatteryType]) continue;
        const int maximum = [source[@kIOPSMaxCapacityKey] intValue];
        if (maximum > 0)
            battery = [source[@kIOPSCurrentCapacityKey] intValue] * 100 / maximum;
        break;
    }
    if (list) CFRelease(list);
    CFRelease(information);
    return battery;
#endif
}

GamepadState ReadGamepad(GCExtendedGamepad* pad)
{
    GamepadState out;
    const struct {
        GCControllerButtonInput* button;
        XInputButton mask;
    } buttons[] = {
        {pad.buttonA, kA}, {pad.buttonB, kB}, {pad.buttonX, kX}, {pad.buttonY, kY},
        {pad.dpad.up, kDpadUp}, {pad.dpad.down, kDpadDown},
        {pad.dpad.left, kDpadLeft}, {pad.dpad.right, kDpadRight},
        {pad.leftShoulder, kLeftShoulder}, {pad.rightShoulder, kRightShoulder},
        {pad.leftThumbstickButton, kLeftThumb}, {pad.rightThumbstickButton, kRightThumb},
        {pad.buttonMenu, kStart}, {pad.buttonOptions, kBack}, {pad.buttonHome, kGuide},
    };
    for (const auto& item : buttons)
        if (item.button.isPressed)
            out.buttons |= item.mask;
    out.left_x = AxisToXInput(pad.leftThumbstick.xAxis.value);
    out.left_y = AxisToXInput(pad.leftThumbstick.yAxis.value);
    out.right_x = AxisToXInput(pad.rightThumbstick.xAxis.value);
    out.right_y = AxisToXInput(pad.rightThumbstick.yAxis.value);
    out.left_trigger = static_cast<std::uint8_t>(std::lround(pad.leftTrigger.value * 255.0f));
    out.right_trigger = static_cast<std::uint8_t>(std::lround(pad.rightTrigger.value * 255.0f));
    return out;
}

struct GamepadFeed::Delivery
{
    GamepadFeed::Sink sink;
    std::atomic<bool> open {true};
};

GamepadFeed::GamepadFeed(Sink sink) : delivery_(std::make_shared<Delivery>())
{
    delivery_->sink = std::move(sink);
    const auto attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    queue_ = dispatch_queue_create("app.lunaone.gamepad", attributes);
}

GamepadFeed::~GamepadFeed()
{
    Detach();
    delivery_->open.store(false);
    // Let a handler already running finish before the sink's owner goes away.
    dispatch_sync(queue_, ^{});
}

void GamepadFeed::Detach()
{
    if (!controller_)
        return;
    controller_.extendedGamepad.valueChangedHandler = nil;
    controller_.handlerQueue = previous_queue_ ?: dispatch_get_main_queue();
    controller_ = nil;
    previous_queue_ = nil;
}

bool GamepadFeed::Follow(GCController* controller)
{
    if (!controller.extendedGamepad)
        controller = nil;
    if (started_ && controller == controller_)
        return controller != nil;
    started_ = true;
    Detach();
    const std::weak_ptr<Delivery> weak = delivery_;
    if (!controller)
    {
        // Release every button the previous controller may have held.
        dispatch_async(queue_, ^{
            if (const auto delivery = weak.lock(); delivery && delivery->open.load())
                delivery->sink({});
        });
        return false;
    }
    controller_ = controller;
    previous_queue_ = controller.handlerQueue;
    controller.handlerQueue = queue_;
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* gamepad, GCControllerElement*) {
        if (const auto delivery = weak.lock(); delivery && delivery->open.load())
            delivery->sink(ReadGamepad(gamepad));
    };
    GCExtendedGamepad* pad = controller.extendedGamepad;
    dispatch_async(queue_, ^{
        if (const auto delivery = weak.lock(); delivery && delivery->open.load())
            delivery->sink(ReadGamepad(pad));
    });
    return true;
}

lunanx::input::ControllerFamily ControllerFamilyOf(GCController* controller)
{
    using lunanx::input::ControllerFamily;
    if (!controller) return ControllerFamily::Xbox;
    GCExtendedGamepad* pad = controller.extendedGamepad;
    if (@available(macOS 11.0, iOS 14.0, *))
    {
        if ([pad isKindOfClass:GCDualShockGamepad.class]) return ControllerFamily::PlayStation;
        if ([pad isKindOfClass:GCXboxGamepad.class]) return ControllerFamily::Xbox;
    }
    if (@available(macOS 11.3, iOS 14.5, *))
        if ([pad isKindOfClass:GCDualSenseGamepad.class]) return ControllerFamily::PlayStation;
    NSString* name = [NSString stringWithFormat:@"%@ %@", controller.vendorName ?: @"", controller.productCategory ?: @""];
    return lunanx::input::ControllerFamilyFromName(name.UTF8String ?: "");
}

std::string ButtonLabel(GCController* controller, lunanx::input::PadButton button)
{
    using lunanx::input::PadButton;
    const auto family = ControllerFamilyOf(controller);
    GCExtendedGamepad* pad = controller.extendedGamepad;
    GCControllerButtonInput* face = nil;
    switch (button)
    {
    case PadButton::A: face = pad.buttonA; break;
    case PadButton::B: face = pad.buttonB; break;
    case PadButton::X: face = pad.buttonX; break;
    case PadButton::Y: face = pad.buttonY; break;
    default: break;
    }
    // Face buttons: the symbol GameController draws for this controller
    // tells which letter or shape is printed on it.
    if (@available(macOS 11.0, iOS 14.0, *))
    {
        NSString* symbol = face.sfSymbolsName;
        if ([symbol hasPrefix:@"xmark"]) return "✕";
        if ([symbol hasPrefix:@"circle"]) return "○";
        if ([symbol hasPrefix:@"square"]) return "□";
        if ([symbol hasPrefix:@"triangle"]) return "△";
        if (symbol.length > 2 && [symbol characterAtIndex:1] == '.')
            return [symbol substringToIndex:1].uppercaseString.UTF8String ?: "";
    }
    return lunanx::input::PadButtonName(family, button);
}

GCController* ActiveGamepad()
{
    GCController* current = GCController.current;
    if (current.extendedGamepad)
        return current;
    for (GCController* controller in GCController.controllers)
        if (controller.extendedGamepad)
            return controller;
    return nil;
}

bool Present(AVSampleBufferDisplayLayer* layer, CVImageBufferRef image)
{
    id<AVQueuedSampleBufferRendering> renderer = layer;
    if (@available(macOS 14.0, iOS 17.0, *))
    {
        auto videoRenderer = layer.sampleBufferRenderer;
        if (videoRenderer.requiresFlushToResumeDecoding ||
            videoRenderer.status == AVQueuedSampleBufferRenderingStatusFailed)
            [videoRenderer flush];
        renderer = videoRenderer;
    }
    else if (layer.status == AVQueuedSampleBufferRenderingStatusFailed)
        [layer flush];
    if (!renderer.readyForMoreMediaData)
        return false;

    CMVideoFormatDescriptionRef format = nullptr;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, image, &format) != noErr)
        return false;
    CMSampleBufferRef sample = nullptr;
    const CMSampleTimingInfo timing {kCMTimeInvalid, kCMTimeZero, kCMTimeInvalid};
    const auto result = CMSampleBufferCreateReadyWithImageBuffer(
        kCFAllocatorDefault, image, format, &timing, &sample);
    CFRelease(format);
    if (result != noErr)
        return false;

    auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    if (attachments && CFArrayGetCount(attachments) > 0)
    {
        auto dict = static_cast<CFMutableDictionaryRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
        CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    }
    [renderer enqueueSampleBuffer:sample];
    CFRelease(sample);
    return true;
}

} // namespace lunaone::apple
