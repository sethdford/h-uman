# SeaClaw Design Tokens

W3C Design Tokens v2025.10 format — single source of truth for all platform styling.

## Token Files

| File                      | Contents                                              |
| ------------------------- | ----------------------------------------------------- |
| `base.tokens.json`        | Colors, spacing, radii, shadows, z-indices            |
| `semantic.tokens.json`    | Semantic color roles (accent, bg, text, border)       |
| `typography.tokens.json`  | Font families, sizes, weights, tracking, leading      |
| `motion.tokens.json`      | Duration, easing, spring curves                       |
| `elevation.tokens.json`   | Shadow depth levels                                   |
| `glass.tokens.json`       | Glassmorphism tiers (surface, elevated, floating)     |
| `opacity.tokens.json`     | Opacity scales                                        |
| `breakpoints.tokens.json` | Responsive breakpoints                                |
| `components.tokens.json`  | Component-specific tokens (button, card, modal, etc.) |

## Build Pipeline

```bash
npm run build       # generates all platform outputs
npm run docs        # generates reference documentation
npm run check:drift # verifies outputs match source tokens
```

### Generated Outputs

| Output                | Path                                            | Platform            |
| --------------------- | ----------------------------------------------- | ------------------- |
| CSS custom properties | `ui/src/styles/_tokens.css`                     | Web                 |
| CSS custom properties | `website/src/styles/_tokens.css`                | Website             |
| Swift constants       | `seaclaw/Sources/SeaClawKit/DesignTokens.swift` | iOS/macOS           |
| Kotlin constants      | (Android module)                                | Android             |
| C `#define` macros    | `include/seaclaw/design_tokens.h`               | C runtime (TUI/CLI) |
| JSON reference        | `docs/design-tokens-reference.json`             | Documentation       |

### How It Works

1. `build.ts` reads all `*.tokens.json` files
2. Walks the W3C token tree, resolving `{references}`
3. Emits platform-specific output files
4. `check-drift.sh` diffs generated outputs against committed versions to detect stale tokens

## Adding Tokens

1. Add the token to the appropriate `*.tokens.json` file
2. Run `npm run build` to regenerate outputs
3. Verify with `npm run check:drift`
4. Commit both the source token file and all generated outputs

## CI Integration

The `design-tokens` job in `.github/workflows/ci.yml` runs `npm run build` and `check-drift.sh` on every PR to ensure tokens stay in sync.
