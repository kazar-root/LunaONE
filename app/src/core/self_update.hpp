// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "update_check.hpp"

namespace lunanx
{
// Whether this copy of LunaONE can replace itself with `update`: the release
// has this computer's file, and LunaONE may write where it is installed.
// Always false on iPhone, iPad and Android (Android goes through its installer).
bool CanInstallUpdate(const UpdateInfo& update);

// Downloads and checks the update, then puts the new files in place of the
// running ones. The new version starts as soon as this process ends, so the
// caller quits right after. On failure it throws a translated message and
// leaves the installed version as it was.
void InstallUpdate(const UpdateInfo& update);

// Windows: a new version started by InstallUpdate waits for the old one to
// end, then removes the files it replaced. Call it first thing in wWinMain.
void CompletePendingUpdate();
}
