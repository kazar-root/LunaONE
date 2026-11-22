// SPDX-License-Identifier: GPL-3.0-only

import SwiftUI
import WatchConnectivity

// Luna's button masks (Xbox layout), as the iPhone sends them to the game.
enum LunaButton {
    static let dpadUp: UInt16 = 0x0001
    static let dpadDown: UInt16 = 0x0002
    static let start: UInt16 = 0x0010
    static let a: UInt16 = 0x1000
    static let b: UInt16 = 0x2000
}

// The link with LunaONE on the iPhone: pictures in, buttons out.
final class MirrorSession: NSObject, ObservableObject, WCSessionDelegate {
    @Published private(set) var picture: UIImage?
    @Published private(set) var title = ""
    @Published private(set) var playing = false
    @Published private(set) var reachable = false

    private var held: UInt16 = 0

    override init() {
        super.init()
        guard WCSession.isSupported() else { return }
        WCSession.default.delegate = self
        WCSession.default.activate()
    }

    // Presses or releases a button; the iPhone receives the whole mask.
    func set(_ button: UInt16, pressed: Bool) {
        let mask = pressed ? held | button : held & ~button
        guard mask != held else { return }
        held = mask
        guard WCSession.default.isReachable else { return }
        WCSession.default.sendMessage(["b": Int(held)], replyHandler: nil, errorHandler: nil)
    }

    // A short press, for the Digital Crown.
    func tap(_ button: UInt16) {
        set(button, pressed: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { [weak self] in
            self?.set(button, pressed: false)
        }
    }

    // MARK: WCSessionDelegate

    func session(_ session: WCSession, activationDidCompleteWith state: WCSessionActivationState, error: Error?) {
        update(reachable: session.isReachable)
    }

    func sessionReachabilityDidChange(_ session: WCSession) {
        update(reachable: session.isReachable)
    }

    // One picture of the game; the answer lets the iPhone send the next one.
    func session(_ session: WCSession, didReceiveMessageData data: Data, replyHandler: @escaping (Data) -> Void) {
        let image = UIImage(data: data)
        DispatchQueue.main.async {
            if let image {
                self.picture = image
                self.playing = true
            }
            replyHandler(Data())
        }
    }

    // { "title": … } when a game starts, { "state": "idle" } when it ends.
    func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        DispatchQueue.main.async {
            if let title = message["title"] as? String {
                self.title = title
                self.playing = true
            }
            if message["state"] as? String == "idle" {
                self.playing = false
                self.picture = nil
                self.held = 0
            }
        }
    }

    private func update(reachable: Bool) {
        DispatchQueue.main.async { self.reachable = reachable }
    }
}
