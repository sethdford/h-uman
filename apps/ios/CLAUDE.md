# apps/ios/ — iOS App

SwiftUI app using HumanKit shared library.

## Rules

- Import and use `HUTokens` from `DesignTokens.swift` for all colors, spacing, radius, fonts, springs.
- Colors: `HUTokens.Dark.*` or `HUTokens.Light.*` — never `Color(red:green:blue:)` or `Color(hex:)`.
- Spacing: `HUTokens.spaceXs` through `HUTokens.space2xl` — never hardcode CGFloat spacing.
- Radius: `HUTokens.radiusSm` through `HUTokens.radiusXl` — never hardcode `.cornerRadius(12)`.
- Font: `Font.custom("Avenir-Book", size:)` / `"Avenir-Medium"` / `"Avenir-Heavy"` / `"Avenir-Black"`.
- Animation: `HUTokens.springMicro`, `HUTokens.springStandard`, `HUTokens.springExpressive`, `HUTokens.springDramatic`.
- Accessibility: support Dynamic Type, VoiceOver, reduce motion.
