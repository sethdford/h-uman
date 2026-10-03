import AppKit
import HumanChatUI
import SwiftUI

/// What the voice HUD shows. Plain values, so every state can be rendered and checked
/// without a running voice session.
struct VoiceHUDState: Equatable {
    enum Mode: Equatable {
        case connecting
        case listening
        case hearing
        case thinking
        case speaking
        case muted
        case failed
    }

    var mode: Mode
    /// Microphone level while hearing, reply level while speaking; 0...1.
    var level: CGFloat = 0
    var title: String
    var detail: String = ""
}

/// The floating voice bar: an animated orb, a status line, and the latest words heard
/// or spoken. Controls appear on hover.
struct VoiceHUDView: View {
    static let size = CGSize(width: HUTokens.space2xl * 7, height: HUTokens.space2xl * 1.5)
    static let corner = HUTokens.radiusXl * 1.5

    let state: VoiceHUDState
    /// Off for offscreen rendering, where an AppKit material cannot be drawn.
    var useMaterial = true
    var onToggleMute: () -> Void = {}
    var onClose: () -> Void = {}

    @State private var hovering = false

    var body: some View {
        HStack(spacing: HUTokens.spaceMd) {
            VoiceOrb(mode: state.mode, level: state.level)
                .frame(width: HUTokens.space2xl, height: HUTokens.space2xl)
            VStack(alignment: .leading, spacing: HUTokens.spaceXs / 2) {
                Text(state.title)
                    .font(.custom("Avenir-Heavy", size: HUTokens.textSm))
                    .foregroundStyle(HUTokens.Dark.text)
                if !state.detail.isEmpty {
                    Text(state.detail)
                        .font(.custom("Avenir-Book", size: HUTokens.textXs))
                        .foregroundStyle(HUTokens.Dark.textMuted)
                        .lineLimit(2)
                        .truncationMode(.tail)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .animation(HUTokens.springStandard, value: state.detail)
        }
        .padding(.leading, HUTokens.spaceMd)
        .padding(.trailing, HUTokens.spaceLg)
        .overlay(alignment: .trailing) {
            HStack(spacing: HUTokens.spaceXs) {
                HUDButton(symbol: state.mode == .muted ? "mic.slash.fill" : "mic.fill",
                          label: state.mode == .muted ? "Unmute microphone" : "Mute microphone",
                          action: onToggleMute)
                HUDButton(symbol: "xmark", label: "Turn off voice mode", action: onClose)
            }
            .padding(.trailing, HUTokens.spaceMd)
            .padding(.leading, HUTokens.space2xl)
            .frame(maxHeight: .infinity)
            .background(
                LinearGradient(colors: [.clear, HUTokens.Dark.bg.opacity(0.85), HUTokens.Dark.bg.opacity(0.95)],
                               startPoint: .leading, endPoint: .trailing)
            )
            .opacity(hovering ? 1 : 0)
            .animation(HUTokens.springMicro, value: hovering)
        }
        .frame(width: Self.size.width, height: Self.size.height)
        .background(background)
        .clipShape(RoundedRectangle(cornerRadius: Self.corner, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Self.corner, style: .continuous)
                .strokeBorder(HUTokens.Dark.text.opacity(0.12), lineWidth: 1)
        )
        .environment(\.colorScheme, .dark)
        .onHover { hovering = $0 }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Human voice: \(state.title)")
    }

    @ViewBuilder private var background: some View {
        if useMaterial {
            ZStack {
                HUDMaterial()
                HUTokens.Dark.bg.opacity(0.35)
            }
        } else {
            HUTokens.Dark.surfaceContainerHigh
        }
    }
}

private struct HUDButton: View {
    let symbol: String
    let label: String
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: HUTokens.textXs, weight: .semibold))
                .foregroundStyle(HUTokens.Dark.text)
                .frame(width: HUTokens.spaceLg + HUTokens.spaceXs,
                       height: HUTokens.spaceLg + HUTokens.spaceXs)
                .background(Circle().fill(HUTokens.Dark.text.opacity(hovering ? 0.18 : 0.08)))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .accessibilityLabel(label)
    }
}

/// Layered light that breathes while listening, follows your voice while you talk,
/// swirls while thinking and pulses with the reply.
struct VoiceOrb: View {
    let mode: VoiceHUDState.Mode
    let level: CGFloat
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    /// One color family per state, so the light mixes instead of turning muddy.
    private var palette: [Color] {
        switch mode {
        case .listening: return [HUTokens.Dark.accent, HUTokens.Dark.accentStrong, HUTokens.Dark.accentHover]
        case .hearing: return [HUTokens.Dark.accentHover, HUTokens.Dark.accentStrong, HUTokens.Dark.accent]
        case .thinking: return [HUTokens.Dark.accentTertiary, HUTokens.Dark.accentTertiaryStrong, HUTokens.Dark.accentTertiaryHover]
        case .speaking: return [HUTokens.Dark.accentSecondary, HUTokens.Dark.accentSecondaryStrong, HUTokens.Dark.accentSecondaryHover]
        case .connecting, .muted: return [HUTokens.Dark.textMuted, HUTokens.Dark.border, HUTokens.Dark.textMuted]
        case .failed: return [HUTokens.Dark.error, HUTokens.Dark.errorDim, HUTokens.Dark.error]
        }
    }

    private var speed: Double {
        switch mode {
        case .thinking: return 1.6
        case .hearing, .speaking: return 0.9
        default: return 0.35
        }
    }

    var body: some View {
        TimelineView(.animation(paused: reduceMotion)) { context in
            let t = reduceMotion ? 0 : context.date.timeIntervalSinceReferenceDate * speed
            let breathe = reduceMotion ? 0 : (sin(t * 1.3) + 1) / 2
            let energy = min(1, max(0, level))
            let scale = 0.86 + 0.06 * breathe + 0.18 * energy
            GeometryReader { geo in
                let d = min(geo.size.width, geo.size.height)
                ZStack {
                    // Deep base so the colors read as light inside glass, not a flat disc.
                    Circle().fill(RadialGradient(colors: [palette[1], palette[0].opacity(0.55)],
                                                 center: .center, startRadius: 0, endRadius: d * 0.6))
                    blob(palette[1], d: d * 0.7, angle: t, radius: d * 0.2)
                    blob(palette[2], d: d * 0.56, angle: -t * 1.4 + 2, radius: d * 0.22)
                    blob(palette[0], d: d * 0.46, angle: t * 0.8 + 4, radius: d * 0.14)
                    // Glass: a soft top-left highlight and a darker rim.
                    Circle()
                        .fill(RadialGradient(colors: [HUTokens.Dark.text.opacity(0.32), .clear],
                                             center: UnitPoint(x: 0.32, y: 0.26),
                                             startRadius: 0, endRadius: d * 0.32))
                    Circle()
                        .strokeBorder(RadialGradient(colors: [.clear, HUTokens.Dark.bg.opacity(0.22)],
                                                     center: .center, startRadius: d * 0.36,
                                                     endRadius: d * 0.5), lineWidth: d * 0.08)
                }
                .frame(width: d, height: d)
                .clipShape(Circle())
                .overlay(Circle().strokeBorder(HUTokens.Dark.text.opacity(0.18), lineWidth: 1))
                .shadow(color: palette[0].opacity(0.35 + 0.35 * energy), radius: d * (0.12 + 0.18 * energy))
                .scaleEffect(scale)
                .frame(width: geo.size.width, height: geo.size.height)
            }
        }
        .accessibilityHidden(true)
    }

    private func blob(_ color: Color, d: CGFloat, angle: Double, radius: CGFloat) -> some View {
        Circle()
            .fill(color)
            .frame(width: d, height: d)
            .offset(x: CGFloat(cos(angle)) * radius, y: CGFloat(sin(angle)) * radius)
            .opacity(0.85)
            .blur(radius: d * 0.18)
    }
}

private struct HUDMaterial: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let v = NSVisualEffectView()
        v.material = .hudWindow
        v.blendingMode = .behindWindow
        v.state = .active
        return v
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}
