import SwiftUI

public struct SCGlassModifier: ViewModifier {
    public enum Tier { case subtle, standard, prominent }
    let tier: Tier

    public func body(content: Content) -> some View {
        #if swift(>=6.2)
        if #available(iOS 26.0, macOS 26.0, *) {
            content.glassEffect(.regular, in: .rect(cornerRadius: glassRadius))
        } else {
            content.background(.ultraThinMaterial).clipShape(RoundedRectangle(cornerRadius: glassRadius))
        }
        #else
        content.background(.ultraThinMaterial).clipShape(RoundedRectangle(cornerRadius: glassRadius))
        #endif
    }

    private var glassRadius: CGFloat {
        switch tier {
        case .subtle: return HUTokens.radiusMd
        case .standard: return HUTokens.radiusLg
        case .prominent: return HUTokens.radiusXl
        }
    }
}

public extension View {
    func scGlass(_ tier: SCGlassModifier.Tier = .standard) -> some View {
        modifier(SCGlassModifier(tier: tier))
    }
}
