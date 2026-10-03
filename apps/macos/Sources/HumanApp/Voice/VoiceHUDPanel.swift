import AppKit
import HumanChatUI
import SwiftUI

/// The always-on-top panel that hosts the HUD. It never takes focus, follows you across
/// Spaces, and can be dragged anywhere; its position is remembered.
@MainActor
final class VoiceHUDPanel {
    private let panel: NSPanel

    init(controller: VoiceModeController) {
        let pad = HUTokens.spaceLg
        let size = NSSize(width: VoiceHUDView.size.width + pad * 2,
                          height: VoiceHUDView.size.height + pad * 2)
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
        let root = VoiceHUDHost(controller: controller)
            .padding(pad)
            .shadow(color: .black.opacity(0.35), radius: HUTokens.spaceMd, y: HUTokens.spaceXs)
        panel.contentView = NSHostingView(rootView: root)
        panel.setFrameAutosaveName("HumanVoiceHUD")
        if !panel.setFrameUsingName("HumanVoiceHUD"), let screen = NSScreen.main {
            let v = screen.visibleFrame
            panel.setFrameOrigin(NSPoint(x: v.midX - size.width / 2, y: v.minY + HUTokens.spaceLg))
        }
    }

    func show() {
        panel.alphaValue = 0
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = HUTokens.durationModerate
            panel.animator().alphaValue = 1
        }
    }

    func hide() {
        NSAnimationContext.runAnimationGroup({ ctx in
            ctx.duration = HUTokens.durationNormal
            panel.animator().alphaValue = 0
        }, completionHandler: { [panel] in
            Task { @MainActor in panel.orderOut(nil) }
        })
    }
}

/// Maps the controller's live state onto the HUD.
private struct VoiceHUDHost: View {
    @ObservedObject var controller: VoiceModeController

    var body: some View {
        VoiceHUDView(state: controller.hudState,
                     onToggleMute: { controller.muted.toggle() },
                     onClose: { controller.stop() })
    }
}
