import SwiftUI

/// View modifier that applies a SwiftUI "glass" material treatment
/// (`.glassEffect` on macOS 26+/iOS 26+, falling back to standard
/// `Material` on older OSes) with an opaque fallback when the user has
/// `accessibilityReduceTransparency` enabled.
@available(macOS 14.0, iOS 17.0, *)
public struct SCGlassModifier: ViewModifier {
    /// Visual intensity of the glass treatment.
    public enum Tier {
        /// Subtle glass (corner radius `radiusMd`, ultra-thin material).
        case subtle
        /// Standard glass (corner radius `radiusLg`, thin material).
        case standard
        /// Prominent glass (corner radius `radiusXl`, regular material).
        case prominent
    }
    let tier: Tier

    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorScheme) private var colorScheme

    public func body(content: Content) -> some View {
        Group {
            if reduceTransparency {
                content.background(solidFill).clipShape(RoundedRectangle(cornerRadius: glassRadius))
            } else {
                #if swift(>=6.2)
                if #available(iOS 26.0, macOS 26.0, *) {
                    switch tier {
                    case .subtle:
                        content.glassEffect(.clear, in: .rect(cornerRadius: glassRadius))
                    case .standard:
                        content.glassEffect(.regular, in: .rect(cornerRadius: glassRadius))
                    case .prominent:
                        content.glassEffect(.regular, in: .rect(cornerRadius: glassRadius))
                    }
                } else {
                    content.background(materialForTier).clipShape(RoundedRectangle(cornerRadius: glassRadius))
                }
                #else
                content.background(materialForTier).clipShape(RoundedRectangle(cornerRadius: glassRadius))
                #endif
            }
        }
    }

    private var materialForTier: Material {
        switch tier {
        case .subtle: return .ultraThinMaterial
        case .standard: return .thinMaterial
        case .prominent: return .regularMaterial
        }
    }

    private var solidFill: Color {
        switch tier {
        case .subtle:
            return colorScheme == .dark ? HUTokens.Dark.surfaceContainer : HUTokens.Light.surfaceContainer
        case .standard:
            return colorScheme == .dark ? HUTokens.Dark.surfaceContainerHigh : HUTokens.Light.surfaceContainerHigh
        case .prominent:
            return colorScheme == .dark ? HUTokens.Dark.surfaceContainerHighest : HUTokens.Light.surfaceContainerHighest
        }
    }

    private var glassRadius: CGFloat {
        switch tier {
        case .subtle: return HUTokens.radiusMd
        case .standard: return HUTokens.radiusLg
        case .prominent: return HUTokens.radiusXl
        }
    }
}

/// `View` convenience for applying `SCGlassModifier`.
@available(macOS 14.0, iOS 17.0, *)
public extension View {
    /// Apply the SCGlass treatment at the given `tier`.
    func scGlass(_ tier: SCGlassModifier.Tier = .standard) -> some View {
        modifier(SCGlassModifier(tier: tier))
    }
}
