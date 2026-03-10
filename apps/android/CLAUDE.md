# apps/android/ — Android App

Jetpack Compose app.

## Rules

- Import and use `HUTokens` from `DesignTokens.kt` for colors, spacing, radius, font sizes.
- Colors: `HUTokens.Dark.*` or `HUTokens.Light.*` — never `Color(0xFF...)`.
- Spacing: `HUTokens.spaceXs` through `HUTokens.space2xl` — never hardcode `16.dp`.
- Radius: `HUTokens.radiusSm` through `HUTokens.radiusXl` — never `RoundedCornerShape(12.dp)`.
- Font sizes: `HUTokens.textXs` through `HUTokens.textXl` — never hardcode `14.sp`.
- Typography: use `AvenirFontFamily` and `AvenirTypography` from `Theme.kt`.
- Theme: use `MaterialTheme.colorScheme.primary` — not `HumanTheme.Coral` directly.
