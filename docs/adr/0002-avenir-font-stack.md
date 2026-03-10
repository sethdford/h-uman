---
title: ADR-0002: Avenir Font Stack
---
# ADR-0002: Avenir Font Stack

- **Status**: accepted
- **Date**: 2025-12-01
- **Deciders**: design system team
- **Tags**: design-system, typography, fonts

## Context

Human needed a consistent typeface across web, iOS, macOS, and Android that conveys precision and modernity without depending on external CDNs.

## Decision

Adopt Avenir as the canonical typeface. Web uses local woff2 files with `@font-face`. Apple platforms use the system-bundled Avenir. Android bundles Avenir TTF files.

## Consequences

### Positive

- Consistent identity across all platforms
- No external CDN dependency
- Excellent readability at all sizes
- Optical size tracking via `--hu-tracking-*` tokens

### Negative

- Font files add ~200KB to web bundle
- Android needs bundled TTF

### Neutral

- Fallback chain includes system-ui and sans-serif

## Alternatives Considered

### Inter

- **Pros**: Popular, well-tested, free
- **Cons**: Generic appearance, typically loaded from Google CDN
- **Why rejected**: Generic look; CDN dependency conflicts with offline/privacy goals

### SF Pro

- **Pros**: Excellent on Apple platforms
- **Cons**: Apple-only licensing and availability
- **Why rejected**: Does not cover web or Android

### System fonts only

- **Pros**: Zero bundle size, native feel
- **Cons**: Inconsistent across platforms (different system fonts)
- **Why rejected**: Inconsistent cross-platform identity

## References

- AGENTS.md §12.1 Typography
- design-tokens/ typography and tracking tokens
