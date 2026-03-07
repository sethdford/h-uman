# Overview Page Bento Rewrite

**Date**: 2026-03-07
**Status**: Approved

## Summary

Rewrite the dashboard overview page from a flat stacked layout to an asymmetric bento grid.
Encodes visual hierarchy through card sizing: health = XL, stats = medium, activity/channels/sessions = varied.

## Grid Layout (desktop 3-col)

```
┌──────────────────────┬─────────────┐
│  System Health (XL)  │  Providers  │
│  Ring + Actions      ├─────────────┤
│                      │  Channels   │
├──────────┬───────────┼─────────────┤
│  Tools   │  Sessions │  Active Ch  │
├──────────┴───────────┤  (2-row)    │
│  Live Activity       │             │
├──────────────────────┴─────────────┤
│  Recent Sessions (full width)      │
└────────────────────────────────────┘
```

## Key Changes

- **Health card**: SVG status ring (animated fill), version, quick actions (Start Chat, Configure)
- **Stat cards**: Composed with sc-sparkline-enhanced + sc-animated-number, accent colors per metric
- **Activity card**: Live pulse indicator, sc-timeline, "Live" badge
- **Channels card**: Compact grid with status dots (not badges)
- **Sessions card**: Horizontal card strip (not a table)
- **Skeleton**: Mirrors bento grid areas

## Responsive

- Tablet (768px): 2 columns, health spans full width
- Mobile (480px): 1 column stack

## Preserved Behavior

- Onboarding welcome flow (not-onboarded shows sc-welcome-card + sc-welcome)
- 30s auto-refresh via GatewayAwareLitElement
- SSE activity event stream
- Scroll entrance animations
