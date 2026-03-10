---
title: Palette Expansion Design
---

# Palette Expansion — Unify on Ocean-Teal + Add Amber & Indigo

**Date:** 2026-03-03
**Status:** Approved

## Problem

The brand feels visually monotone ("too coral"). Root cause: the website and docs use coral (#f97066) as the primary accent, while the design token source of truth already defines teal (#14b8a6) as primary. This divergence means the ocean identity never reaches the public-facing website. Additionally, the palette lacks secondary and tertiary accent colors for visual variety.

## Solution

1. Unify all surfaces on teal as the primary accent (matching the token source of truth).
2. Add two new primitive color ramps — amber and indigo — with full tint scales.
3. Wire them as semantic `accent-secondary` (amber) and `accent-tertiary` (indigo) tokens.
4. Replace coral accent usage in the website CSS with teal + amber + indigo blend.
5. Demote coral to error/destructive states only.

## New Primitives

### Amber (warm secondary accent)

Hex scale: #fffbeb (50) → #f59e0b (500) → #78350f (900)

Use: featured content, warm highlights, CTAs needing contrast from teal, data visualization warm series.

### Indigo (depth/info accent)

Hex scale: #eef2ff (50) → #6366f1 (500) → #312e81 (900)

Use: info states, alternative link color, provider-related UI, data visualization cool series, depth and richness.

## Semantic Token Additions

| Token                     | Dark                  | Light                |
| ------------------------- | --------------------- | -------------------- |
| `accent-secondary`        | amber.500             | amber.600            |
| `accent-secondary-hover`  | amber.400             | amber.700            |
| `accent-secondary-subtle` | rgba(245,158,11,0.14) | rgba(217,119,6,0.10) |
| `accent-secondary-text`   | amber.400             | amber.700            |
| `accent-tertiary`         | indigo.500            | indigo.600           |
| `accent-tertiary-hover`   | indigo.400            | indigo.700           |
| `accent-tertiary-subtle`  | rgba(99,102,241,0.14) | rgba(79,70,229,0.10) |
| `accent-tertiary-text`    | indigo.400            | indigo.700           |

## Website CSS Changes

- `--color-accent` → teal (#14b8a6 light, #2dd4bf dark)
- Hero mesh gradient → teal + indigo + amber blend (replaces coral-dominant conic)
- Text gradient shimmer → teal → amber → indigo cycle
- Card glow → teal/indigo (replaces coral/blue)
- Ambient blobs → `ambient-teal` + `ambient-indigo` (replaces `ambient-coral`)
- Selection color → teal-based
- Terminal cursor → teal

## What Does NOT Change

- Coral ramp stays in `base.tokens.json` (used for error/error-dim semantic tokens)
- Pill colors (provider=blue, channel=green, tool=yellow) — already diverse
- All neutral, seafoam, green, yellow, red, blue primitives
- Typography, spacing, radius, motion, elevation tokens
- Dashboard UI tokens (already use teal via `--hu-accent`)

## Result

5 distinct color voices: teal (primary), amber (warm), indigo (depth), seafoam (success), coral (error-only).

## Files Modified

1. `design-tokens/base.tokens.json` — add amber + indigo ramps
2. `design-tokens/semantic.tokens.json` — add accent-secondary/tertiary
3. `website/src/styles/global.css` — replace coral with teal/amber/indigo
4. `docs/design-system.md` — update color documentation
5. Generated outputs (CSS, Swift, Kotlin, C) — rebuild via `npx tsx design-tokens/build.ts`
