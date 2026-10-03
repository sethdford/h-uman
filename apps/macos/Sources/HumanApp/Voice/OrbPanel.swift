import AppKit
import HumanChatUI
import SwiftUI

/// A small always-on-top orb showing what voice mode is doing. It never takes
/// focus, follows you across Spaces, and can be dragged anywhere.
@MainActor
final class OrbPanel {
    private let panel: NSPanel

    init(controller: VoiceModeController) {
        let size = NSSize(width: OrbView.panelWidth, height: OrbView.panelHeight)
        panel = NSPanel(contentRect: NSRect(origin: .zero, size: size),
                        styleMask: [.nonactivatingPanel, .borderless],
                        backing: .buffered, defer: true)
        panel.level = .floating
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        panel.isMovableByWindowBackground = true
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.hidesOnDeactivate = false
        panel.contentView = NSHostingView(rootView: OrbView(controller: controller))
        panel.setFrameAutosaveName("HumanVoiceOrb")
        if !panel.setFrameUsingName("HumanVoiceOrb"), let screen = NSScreen.main {
            let v = screen.visibleFrame
            panel.setFrameOrigin(NSPoint(x: v.maxX - size.width - HUTokens.spaceLg,
                                         y: v.minY + HUTokens.spaceLg))
        }
    }

    func show() { panel.orderFrontRegardless() }
    func hide() { panel.orderOut(nil) }
}

struct OrbView: View {
    static let orbSize = HUTokens.space2xl * 1.5
    static let panelWidth = HUTokens.space2xl * 5
    static let panelHeight = orbSize + HUTokens.space2xl * 1.5

    @ObservedObject var controller: VoiceModeController
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var pulse = false

    var body: some View {
        VStack(spacing: HUTokens.spaceSm) {
            orb
                .onTapGesture { controller.muted.toggle() }
                .contextMenu {
                    Button(controller.muted ? "Unmute Microphone" : "Mute Microphone") {
                        controller.muted.toggle()
                    }
                    Button("Turn Off Voice Mode") { controller.stop() }
                }
            Text(caption)
                .font(.custom("Avenir-Book", size: HUTokens.textSm))
                .foregroundStyle(HUTokens.Dark.text)
                .lineLimit(2)
                .multilineTextAlignment(.center)
                .padding(.horizontal, HUTokens.spaceSm)
                .padding(.vertical, HUTokens.spaceXs)
                .background(Capsule().fill(HUTokens.Dark.bgSurface.opacity(0.85)))
                .opacity(caption.isEmpty ? 0 : 1)
        }
        .frame(width: Self.panelWidth, height: Self.panelHeight, alignment: .bottom)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Human voice mode: \(statusText)")
        .accessibilityHint("Tap to mute or unmute the microphone")
    }

    private var orb: some View {
        let scale = 1 + (controller.phase == .hearing ? controller.level * 0.25 : 0)
            + (controller.phase == .speaking && !reduceMotion && pulse ? 0.06 : 0)
        return ZStack {
            Circle()
                .fill(RadialGradient(colors: [color.opacity(0.95), color.opacity(0.35)],
                                     center: .center, startRadius: 0,
                                     endRadius: Self.orbSize / 2))
            Circle()
                .stroke(color, lineWidth: HUTokens.spaceXs / 2)
                .opacity(controller.phase == .thinking && !reduceMotion && pulse ? 0.2 : 0.8)
            if controller.muted {
                Image(systemName: "mic.slash.fill")
                    .font(.system(size: HUTokens.textLg))
                    .foregroundStyle(HUTokens.Dark.text)
            }
        }
        .frame(width: Self.orbSize, height: Self.orbSize)
        .scaleEffect(scale)
        .animation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.86), value: scale)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true)) { pulse = true }
        }
    }

    private var color: Color {
        if controller.muted { return HUTokens.Dark.textMuted }
        switch controller.phase {
        case .off, .connecting: return HUTokens.Dark.textMuted
        case .listening, .hearing: return HUTokens.Dark.accent
        case .thinking: return HUTokens.Dark.accentTertiary
        case .speaking: return HUTokens.Dark.accentSecondary
        case .failed: return HUTokens.Dark.error
        }
    }

    private var statusText: String {
        if controller.muted { return "muted" }
        switch controller.phase {
        case .off: return "off"
        case .connecting: return "connecting to the voice gateway"
        case .listening: return "listening"
        case .hearing: return "hearing you"
        case .thinking: return "thinking"
        case .speaking: return "speaking"
        case .failed(let why): return why
        }
    }

    private var caption: String {
        switch controller.phase {
        case .speaking: return controller.said
        case .thinking: return controller.heard
        case .connecting, .failed: return statusText
        default: return controller.muted ? "Muted" : ""
        }
    }
}
