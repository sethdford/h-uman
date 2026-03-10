# apps/android/ — Android App

Jetpack Compose app.

## Rules

- Import and use `SCTokens` from `DesignTokens.kt` for colors, spacing, radius, font sizes.
- Colors: `SCTokens.Dark.*` or `SCTokens.Light.*` — never `Color(0xFF...)`.
- Spacing: `SCTokens.spaceXs` through `SCTokens.space2xl` — never hardcode `16.dp`.
- Radius: `SCTokens.radiusSm` through `SCTokens.radiusXl` — never `RoundedCornerShape(12.dp)`.
- Font sizes: `SCTokens.textXs` through `SCTokens.textXl` — never hardcode `14.sp`.
- Typography: use `AvenirFontFamily` and `AvenirTypography` from `Theme.kt`.
- Theme: use `MaterialTheme.colorScheme.primary` — not `HumanTheme.Coral` directly.
