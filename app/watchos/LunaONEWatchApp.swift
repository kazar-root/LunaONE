// SPDX-License-Identifier: GPL-3.0-only

import SwiftUI

// LunaONE on Apple Watch: the game played on the paired iPhone, mirrored on
// the wrist, with A, B, the Digital Crown and a long press as controls.
@main
struct LunaONEWatchApp: App {
    @StateObject private var mirror = MirrorSession()

    var body: some Scene {
        WindowGroup {
            MirrorView()
                .environmentObject(mirror)
        }
    }
}
