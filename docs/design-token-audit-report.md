---
title: Design Token Audit Report
---

# Design Token Audit Report

**Date:** 2026-03-07  
**Scope:** All consumers — ui, website, docs, design-tokens (excluding node_modules, dist, \_tokens.css)

## Summary

| Metric                               | Count |
| ------------------------------------ | ----- |
| **Total tokens defined**             | 367   |
| **Tokens used** (direct or indirect) | 274   |
| **Truly unused**                     | 173   |

## Categorization

### 1. Truly Unused (173 tokens)

These tokens are **not referenced** in ui/src, website/src, docs, or design-tokens. They are also **not used as values** in other tokens (e.g. `var(--hu-x)` inside another token's definition).

**Note:** Base tokens in `base.tokens.json` (e.g. `color.neutral.50`) are **not** emitted as CSS vars — they are resolved into semantic tokens during build. They do not appear in this audit.

### 2. Used Indirectly (1 token)

- `--hu-accent-subtle` — referenced inside `--hu-shadow-glow-accent`, which is used in `website/src/styles/global.css`

### 3. Platform-Specific Consumers

- **C** (`include/human/design_tokens.h`): Uses `HU_COLOR_*` macros — hand-maintained, not generated from CSS var names.
- **Apps** (Flutter, iOS, Android): Use generated `HUTokens` / `DesignTokens` — same source JSON, different output format. Removing a token from source would affect all platforms.

## Full List of Truly Unused Tokens

### Avatar (5)

- `--hu-avatar-font-size`, `--hu-avatar-font-weight`
- `--hu-avatar-size-lg`, `--hu-avatar-size-md`, `--hu-avatar-size-sm`

### Blur (2)

- `--hu-blur-lg`, `--hu-blur-xl`

### Breakpoints (5)

- `--hu-breakpoint-2xl`, `--hu-breakpoint-lg`, `--hu-breakpoint-md`, `--hu-breakpoint-sm`, `--hu-breakpoint-xl`

### Choreography (1)

- `--hu-cascade-max`

### Command Palette (6)

- `--hu-command-palette-item-padding-x`, `--hu-command-palette-item-padding-y`, `--hu-command-palette-item-radius`
- `--hu-command-palette-max-width`, `--hu-command-palette-padding`, `--hu-command-palette-radius`

### Container Transform (5)

- `--hu-container-transform-duration`, `--hu-container-transform-easing`, `--hu-container-transform-fade-through-duration`
- `--hu-container-transform-scale-end`, `--hu-container-transform-scale-start`

### Dropdown (1)

- `--hu-dropdown-radius`

### Elevation (4)

- `--hu-elevation-0`, `--hu-elevation-2`, `--hu-elevation-4`, `--hu-elevation-5`

### Motion — Emphasize (2)

- `--hu-emphasize-duration`, `--hu-emphasize-easing`

### Motion — Enter/Exit (4)

- `--hu-enter-duration`, `--hu-enter-easing`, `--hu-exit-duration`, `--hu-exit-easing`

### Floating Action Button (3)

- `--hu-floating-action-button-icon-size`, `--hu-floating-action-button-radius`, `--hu-floating-action-button-size`

### Glass (9)

- `--hu-glass-dynamic-light-ambient`, `--hu-glass-dynamic-light-angle`
- `--hu-glass-interactive-press-saturate-boost`
- `--hu-glass-prominent-refraction-scale`, `--hu-glass-standard-refraction-scale`
- `--hu-glass-subtle-inset-opacity`, `--hu-glass-subtle-refraction-scale`
- `--hu-glass-vibrancy-backdrop-brightness`, `--hu-glass-vibrancy-icon-boost`, `--hu-glass-vibrancy-text-boost`

### Input (4)

- `--hu-input-font-size`, `--hu-input-padding-x`, `--hu-input-padding-y`, `--hu-input-radius`

### Motion — Move (2)

- `--hu-move-duration`, `--hu-move-easing`

### Opacity (7)

- `--hu-opacity-dragged`, `--hu-opacity-focus`, `--hu-opacity-hover`
- `--hu-opacity-overlay-heavy`, `--hu-opacity-overlay-light`, `--hu-opacity-overlay-medium`, `--hu-opacity-pressed`

### Motion — Page (4)

- `--hu-page-enter-duration`, `--hu-page-enter-easing`, `--hu-page-exit-duration`, `--hu-page-exit-easing`

### Path Motion (4)

- `--hu-path-motion-arc-duration`, `--hu-path-motion-arc-easing`
- `--hu-path-motion-offset-distance-end`, `--hu-path-motion-offset-distance-start`

### Physics (6)

- `--hu-physics-dropdown-anticipate-scaleY`, `--hu-physics-modal-anticipate-pause`
- `--hu-physics-secondary-damping`, `--hu-physics-secondary-delay`
- `--hu-physics-toggle-squash-scaleX`, `--hu-physics-toggle-squash-scaleY`

### Progress (2)

- `--hu-progress-height`, `--hu-progress-radius`

### Shadow (1)

- `--hu-shadow-inset`

### Motion — Shared Element (2)

- `--hu-shared-element-duration`, `--hu-shared-element-easing`

### Sheet (4)

- `--hu-sheet-handle-height`, `--hu-sheet-handle-width`, `--hu-sheet-padding`, `--hu-sheet-radius`

### Spring (8)

- `--hu-spring-dramatic-damping`, `--hu-spring-dramatic-stiffness`
- `--hu-spring-expressive-damping`, `--hu-spring-expressive-stiffness`
- `--hu-spring-micro-damping`, `--hu-spring-micro-stiffness`
- `--hu-spring-standard-damping`, `--hu-spring-standard-stiffness`

### Choreography (1)

- `--hu-stagger-max`

### Surface (1)

- `--hu-surface-glow`

### Tabs (5)

- `--hu-tabs-font-size`, `--hu-tabs-font-weight`, `--hu-tabs-indicator-height`
- `--hu-tabs-padding-x`, `--hu-tabs-padding-y`

### Tooltip (5)

- `--hu-tooltip-font-size`, `--hu-tooltip-max-width`, `--hu-tooltip-padding-x`, `--hu-tooltip-padding-y`, `--hu-tooltip-radius`

### Type Roles — Body (12)

- `--hu-type-body-lg-*`, `--hu-type-body-md-*`, `--hu-type-body-sm-*` (size, weight, letter-spacing, line-height each)

### Type Roles — Caption (4)

- `--hu-type-caption-size`, `--hu-type-caption-weight`, `--hu-type-caption-letter-spacing`, `--hu-type-caption-line-height`

### Type Roles — Display (8)

- `--hu-type-display-lg-*`, `--hu-type-display-md-*` (size, weight, letter-spacing, line-height each)

### Type Roles — Headline (12)

- `--hu-type-headline-lg-*`, `--hu-type-headline-md-*`, `--hu-type-headline-sm-*` (size, weight, letter-spacing, line-height each)

### Type Roles — Label (12)

- `--hu-type-label-lg-*`, `--hu-type-label-md-*`, `--hu-type-label-sm-*` (size, weight, letter-spacing, line-height each)

### Type Roles — Title (12)

- `--hu-type-title-lg-*`, `--hu-type-title-md-*`, `--hu-type-title-sm-*` (size, weight, letter-spacing, line-height each)

### View Transition (4)

- `--hu-view-transition-duration`, `--hu-view-transition-easing`
- `--hu-view-transition-new-enter`, `--hu-view-transition-old-exit`

### Z-Index (5)

- `--hu-z-base`, `--hu-z-modal`, `--hu-z-sticky`, `--hu-z-toast`, `--hu-z-tooltip`

## Is It Safe to Remove?

**Before removing any token from the source JSON files:**

1. **Apps dependency:** Flutter, iOS, and Android consume the same `design-tokens/*.json` source. Removing a token will affect generated `HUTokens` / `DesignTokens` in those platforms. Verify app usage first (e.g. `HUTokens.avatarSizeLg` in Swift, `HUTokens.avatar_size_lg` in Dart).

2. **Reserved for future use:** Tokens like `--hu-z-modal`, `--hu-z-toast`, `--hu-z-tooltip` may be intended for components not yet built. Check roadmap/design docs.

3. **Type roles:** The `--hu-type-*` tokens are expanded typography roles. Components might use `--hu-text-base` or `--hu-font` instead of the granular type-body-lg-size. Consider whether to adopt type roles in components or remove them.

4. **Recommended approach:** Remove tokens in small batches, run the design-tokens build, then run app builds and tests to catch breakages.

## Audit Script

The script `ui/scripts/audit-unused-tokens-all-consumers.sh` performs this audit. It was updated to exclude `node_modules`, `dist`, and `_tokens.css` from the search for accuracy and speed.
