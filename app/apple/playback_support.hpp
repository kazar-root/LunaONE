// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "input/controller_labels.hpp"
#include "input/luna_input_encoder.hpp"

#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#import <Network/Network.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace lunaone::apple
{

lunanx::input::GamepadState ReadGamepad(GCExtendedGamepad* pad);
GCController* ActiveGamepad();
// Xbox, PlayStation or Nintendo, for the button names shown on screen.
lunanx::input::ControllerFamily ControllerFamilyOf(GCController* controller);
// The label printed on a button of this controller ("A", "✕", "B"…), as
// GameController reports it, or the family's name for that position.
std::string ButtonLabel(GCController* controller, lunanx::input::PadButton button);

// Delivers each controller change from GameController's handler queue as it
// happens, instead of waiting for the next UI timer or display refresh. All
// states, including the released state when the controller goes away, reach
// the sink in order on one serial high-priority queue.
class GamepadFeed
{
  public:
    using Sink = std::function<void(const lunanx::input::GamepadState&)>;
    explicit GamepadFeed(Sink sink);
    ~GamepadFeed();
    GamepadFeed(const GamepadFeed&) = delete;
    GamepadFeed& operator=(const GamepadFeed&) = delete;

    // Main thread. Follows the active controller and returns whether one is attached.
    bool Follow(GCController* controller);

  private:
    void Detach();

    struct Delivery;
    std::shared_ptr<Delivery> delivery_;
    dispatch_queue_t queue_;
    GCController* controller_ = nil;
    dispatch_queue_t previous_queue_ = nil;
    bool started_ = false;
};
bool Present(AVSampleBufferDisplayLayer* layer, CVImageBufferRef image);
void FetchLaunchCover(const std::string& address, void (^completion)(NSData*));

class NetworkStatus
{
  public:
    NetworkStatus();
    ~NetworkStatus();
    std::string Label() const;

  private:
    nw_path_monitor_t monitor_;
    std::shared_ptr<std::atomic_int> transport_;
};

int SystemBatteryPercent();

} // namespace lunaone::apple
