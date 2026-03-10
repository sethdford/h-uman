# apps/ios/ — iOS App

SwiftUI app using HumanKit shared library.

## Rules

- Import and use `SCTokens` from `DesignTokens.swift` for all colors, spacing, radius, fonts, springs.
- Colors: `SCTokens.Dark.*` or `SCTokens.Light.*` — never `Color(red:green:blue:)` or `Color(hex:)`.
- Spacing: `SCTokens.spaceXs` through `SCTokens.space2xl` — never hardcode CGFloat spacing.
- Radius: `SCTokens.radiusSm` through `SCTokens.radiusXl` — never hardcode `.cornerRadius(12)`.
- Font: `Font.custom("Avenir-Book", size:)` / `"Avenir-Medium"` / `"Avenir-Heavy"` / `"Avenir-Black"`.
- Animation: `SCTokens.springMicro`, `SCTokens.springStandard`, `SCTokens.springExpressive`, `SCTokens.springDramatic`.
- Accessibility: support Dynamic Type, VoiceOver, reduce motion.
