// SPDX-License-Identifier: GPL-3.0-only

import SwiftUI

private let violet = Color(red: 0.67, green: 0.53, blue: 1.0)

struct MirrorView: View {
    @EnvironmentObject private var mirror: MirrorSession
    @State private var crown = 0.0
    @State private var crownStep = 0

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.10, green: 0.08, blue: 0.19), .black],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            if let picture = mirror.picture {
                game(picture)
            } else {
                waiting
            }
        }
        .focusable()
        // The Digital Crown moves through the game's menus: up and down on the D-pad.
        .digitalCrownRotation($crown, from: -1000, through: 1000, by: 1, sensitivity: .medium,
                              isContinuous: true, isHapticFeedbackEnabled: true)
        .onChange(of: crown) { _, value in
            let step = Int(value.rounded())
            guard step != crownStep else { return }
            mirror.tap(step > crownStep ? LunaButton.dpadDown : LunaButton.dpadUp)
            crownStep = step
        }
    }

    private var waiting: some View {
        VStack(spacing: 8) {
            Image("LunaONELogo")
                .resizable()
                .scaledToFit()
                .frame(width: 70, height: 70)
                .clipShape(RoundedRectangle(cornerRadius: 16))
            Text("LunaONE")
                .font(.headline)
            Text(mirror.reachable ? "Lance un jeu sur l’iPhone" : "Ouvre LunaONE sur l’iPhone")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding()
    }

    private func game(_ picture: UIImage) -> some View {
        VStack(spacing: 6) {
            Image(uiImage: picture)
                .resizable()
                .scaledToFit()
                .clipShape(RoundedRectangle(cornerRadius: 6))
                // A long press on the picture opens the game's menu (Start).
                .onLongPressGesture(minimumDuration: 0.6) { mirror.tap(LunaButton.start) }
            if !mirror.title.isEmpty {
                Text(mirror.title)
                    .font(.caption2)
                    .lineLimit(1)
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 18) {
                padButton("B", LunaButton.b, color: Color(red: 0.86, green: 0.25, blue: 0.30))
                padButton("A", LunaButton.a, color: Color(red: 0.30, green: 0.75, blue: 0.40))
            }
        }
        .padding(.horizontal, 4)
    }

    // Held as long as the finger stays on it, like a real button.
    private func padButton(_ label: String, _ button: UInt16, color: Color) -> some View {
        Text(label)
            .font(.system(size: 20, weight: .bold, design: .rounded))
            .frame(width: 46, height: 46)
            .background(Circle().fill(color.opacity(0.85)))
            .overlay(Circle().stroke(violet, lineWidth: 1.5))
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in mirror.set(button, pressed: true) }
                .onEnded { _ in mirror.set(button, pressed: false) })
    }
}
