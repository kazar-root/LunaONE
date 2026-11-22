// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "luna/luna_client.hpp"

#import <UIKit/UIKit.h>
#include <memory>

@interface LunaPlayerController : UIViewController
- (instancetype)initWithClient:(std::shared_ptr<const lunanx::luna::LunaClient>)client
                          game:(lunanx::luna::Game)game;
@end
