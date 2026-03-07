# design-tokens/ — Design Token Pipeline

W3C Design Tokens v2025.10 format. Single source of truth for all platforms.

## Commands

```bash
npm run build          # generate all platform outputs
npm run docs           # generate token reference docs
```

## Rules

- Edit `.tokens.json` files only — never hand-edit generated outputs
- Token format: `$value`, `$type`, `$description` (W3C standard)
- Token names map to `--sc-*` CSS custom properties
- After editing, run `npm run build` to regenerate: CSS, Kotlin, Swift, C, Dart
- `check-drift.sh` verifies generated outputs match source — runs in pre-commit and CI

## Token Files

| File | Purpose |
|------|---------|
| `base.tokens.json` | Color primitives (OKLCH) |
| `typography.tokens.json` | Font sizes, weights, line heights |
| `motion.tokens.json` | Durations, easings, springs |
| `data-viz.tokens.json` | Chart categorical/sequential/diverging colors |
| `elevation.tokens.json` | Shadows and depth |
| `glass.tokens.json` | Glassmorphism effects |
| `breakpoints.tokens.json` | Responsive breakpoints |
| `opacity.tokens.json` | Opacity scale |
| `components.tokens.json` | Component-specific tokens |

## Generated Outputs

- `ui/src/styles/_tokens.css` — CSS custom properties
- `website/src/styles/_tokens.css` — CSS (website)
- `apps/ios/SeaClaw/DesignTokens.swift` — Swift constants
- `apps/android/.../DesignTokens.kt` — Kotlin constants
- `include/seaclaw/design_tokens.h` — C macros
