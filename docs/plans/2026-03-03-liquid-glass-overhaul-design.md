---
title: Liquid Glass Overhaul + SOTA Conversational AI Components
date: 2026-03-03
status: approved
---

# Liquid Glass Overhaul + SOTA Conversational AI Components

## Context

SeaClaw's design system already has a three-tier glass token system (subtle/standard/prominent),
ocean-tonal neutrals, spring physics, and `prefers-reduced-transparency` fallbacks. This design
evolves the system to match Apple's Liquid Glass (WWDC 2025) across all platforms and adds
world-class conversational AI components.

## Goals

1. Glass becomes the **default material** for all elevated surfaces (not an optional prop)
2. Dynamic light response — glass refracts with cursor/gyroscope input
3. Vibrancy — text/icons auto-enhance contrast on glass surfaces
4. Interactive glass — press deformation, hover shimmer, focus glow
5. Conversational AI component suite — streaming, thinking, tool results, branching
6. Native parity — SwiftUI `glassEffect()`, Android `RenderEffect` mappings

## 1. Material System

Replace solid `bg-surface`/`bg-elevated` with glass-backed defaults.

### New tokens (`glass.tokens.json`)

```json
{
  "dynamic-light": {
    "angle": {
      "$value": "225deg",
      "$description": "Default light source angle"
    },
    "intensity": {
      "$value": 0.15,
      "$description": "Specular highlight intensity"
    },
    "spread": { "$value": "60%", "$description": "Specular highlight spread" },
    "color": {
      "$value": "rgba(255,255,255,0.12)",
      "$description": "Specular highlight color"
    }
  },
  "vibrancy": {
    "text-boost": {
      "$value": 1.15,
      "$description": "Text contrast multiplier on glass"
    },
    "icon-boost": {
      "$value": 1.2,
      "$description": "Icon contrast multiplier on glass"
    },
    "backdrop-brightness": {
      "$value": 1.08,
      "$description": "Brightness boost for glass backdrop"
    }
  },
  "interactive": {
    "press-blur-delta": {
      "$value": "-4px",
      "$description": "Blur decreases on press (glass compresses)"
    },
    "press-scale": { "$value": 0.98, "$description": "Scale on press" },
    "hover-specular-boost": {
      "$value": 1.4,
      "$description": "Specular intensity multiplier on hover"
    },
    "focus-glow-spread": {
      "$value": "8px",
      "$description": "Glow spread on focus"
    }
  }
}
```

### CSS: Glass as default surface

Components switch from `background: var(--sc-bg-surface)` to glass:

```css
.sc-surface {
  background: color-mix(
    in srgb,
    var(--sc-bg-surface) calc(100% - var(--sc-glass-standard-bg-opacity) * 100%),
    transparent
  );
  backdrop-filter: blur(var(--sc-glass-standard-blur))
    saturate(var(--sc-glass-standard-saturate));
  -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur))
    saturate(var(--sc-glass-standard-saturate));
}
```

Fallback for `prefers-reduced-transparency: reduce` → solid `var(--sc-bg-surface)`.

## 2. Dynamic Light Response

### JS Module: `ui/src/lib/dynamic-light.ts`

- Tracks cursor position on desktop → maps to `--sc-light-x` and `--sc-light-y` CSS properties
- Uses DeviceOrientation API on mobile → maps gyroscope to light angle
- Updates via `requestAnimationFrame` with throttling
- Specular highlight rendered as CSS `radial-gradient` overlay that follows light source
- Respects `prefers-reduced-motion`

### CSS: Specular overlay

```css
.sc-glass-surface::after {
  content: "";
  position: absolute;
  inset: 0;
  border-radius: inherit;
  background: radial-gradient(
    ellipse var(--sc-light-spread) var(--sc-light-spread) at
      var(--sc-light-x, 50%) var(--sc-light-y, 30%),
    var(--sc-light-color),
    transparent
  );
  pointer-events: none;
  mix-blend-mode: overlay;
}
```

## 3. Vibrancy System

Text and icons on glass surfaces get automatic contrast enhancement:

```css
.sc-glass-surface {
  --sc-vibrancy-text: color-mix(
    in srgb,
    var(--sc-text) calc(var(--sc-vibrancy-text-boost) * 100%),
    white
  );
}

.sc-glass-surface :is(h1, h2, h3, h4, p, span, label) {
  color: var(--sc-vibrancy-text, var(--sc-text));
}
```

Backdrop brightness: `backdrop-filter` includes `brightness(var(--sc-vibrancy-backdrop-brightness))`.

## 4. Interactive Glass

### Press deformation

- Scale to `var(--sc-glass-interactive-press-scale)` (0.98)
- Blur shifts by `var(--sc-glass-interactive-press-blur-delta)` (-4px)
- Spring-micro easing

### Hover shimmer

- Specular intensity multiplied by `var(--sc-glass-interactive-hover-specular-boost)` (1.4)
- Subtle border glow

### Focus glow

- Teal glow ring using `box-shadow` with `var(--sc-glass-interactive-focus-glow-spread)`

## 5. Conversational AI Components

### `sc-message-stream`

- Progressive markdown rendering with streaming support
- Token-by-token appearance animation
- Code blocks with syntax highlighting and copy button
- Image/file embeds inline

### `sc-thinking`

- Collapsible reasoning/thinking indicator
- Animated pulse during active thinking
- Step-by-step display when expanded
- Duration timer

### `sc-tool-result`

- Rich inline display for tool execution results
- Variants: shell output (terminal-styled), file content, code, image, error
- Collapsible with status indicator (running/success/error)
- Glass surface with elevation-2

### `sc-message-branch`

- Conversation fork visualization
- Horizontal branch selector with count indicator
- Edit-and-resubmit support
- Animated transition between branches

### `sc-conversation`

- Container managing message flow layout
- Auto-scroll with "new messages" indicator
- Stagger animation for message entrance
- Glass-subtle sidebar for conversation metadata

## 6. Native Parity

### SwiftUI (iOS 26+)

- `SCGlassModifier` using `.glassEffect()` with `.regular`/`.clear` variants
- Token-mapped blur/saturate values
- Spring animations from `SCTokens.springStandard`

### Android (Material 3)

- `SCGlassEffect` composable using `RenderEffect.createBlurEffect()`
- Token-mapped elevation shadows
- Spring animations from `SCTokens.springStandard*`

## Implementation Order

1. Material tokens — add dynamic-light, vibrancy, interactive to glass.tokens.json
2. Regenerate all platform outputs
3. Dynamic light JS module
4. Glass CSS utility classes in theme.css
5. Update components to glass-default (sc-card, sc-dialog, sc-sheet, sc-modal, sc-dropdown, sc-popover, sc-toast, sc-tooltip)
6. Vibrancy CSS system
7. Interactive glass CSS/JS
8. Conversational AI components
9. Native parity (SwiftUI + Android)
10. Validate build + tests
