// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <functional>

// The game picture on a paired Apple Watch, and the watch's buttons as
// controller input. The iPhone sends a small JPEG once the watch has shown
// the previous one; the watch sends the Luna button mask it holds.
@interface LunaWatchMirror : NSObject
+ (instancetype)shared;
// Called once at launch. Does nothing where WatchConnectivity is missing (iPad).
- (void)activate;
// A game starts: its title goes to the watch, and the watch's buttons to the
// handler until the game ends.
- (void)beginGame:(NSString*)title buttons:(std::function<void(std::uint16_t)>)buttons;
- (void)endGame;
// A decoded picture. Most are skipped while the watch shows the previous one.
- (void)offerFrame:(CVPixelBufferRef)image;
@end
