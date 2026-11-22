// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "luna/luna_client.hpp"
#include <gtk/gtk.h>

namespace lunaone::linux_desktop
{
void Play(GtkWindow* parent, const lunanx::luna::LunaClient& client, lunanx::luna::Game game);
}
