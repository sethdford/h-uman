import SwiftUI

/// Glass-style background modifier with three intensity tiers.
///
/// Falls back to a solid `HUTokens` surface fill when Reduce Transparency is
/// enabled, the `glassEffect` material on iOS 26 / macOS 26, or the
/// corresponding `Material` on earlier OS versions.
@available(macOS 14.0, iOS 17.0, *)
public struct SCGlassModifier: ViewModifier {
    /// Glass intensity tier; controls both material density and corner radius.
    public enum Tier {
        /// Lightest glass over `surfaceContainer`.
        case subtle
        /// Default glass over `surfaceContainerHigh`.
        case standard
        /// Densest glass over `surfaceContainerHighest`.
        case prominent
    }
    let tier: Tier

    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.colorScheme) private var colorScheme

    /// Build the modified view body.
    ///
    /// - Parameter content: The content the modifier is applied to.
    /// - Returns: A glass-backed (or solid-backed) view.
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

@available(macOS 14.0, iOS 17.0, *)
public extension View {
    /// Apply the `SCGlassModifier` at the given tier.
    ///
    /// - Parameter tier: Intensity tier (defaults to `.standard`).
    /// - Returns: The view backed by the glass material.
    func scGlass(_ tier: SCGlassModifier.Tier = .standard) -> some View {
        modifier(SCGlassModifier(tier: tier))
    }
}
