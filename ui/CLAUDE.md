# ui/ — Web Dashboard

LitElement web components for the seaclaw dashboard.

## Rules

- Component tag: `sc-<name>`, registered with `@customElement("sc-<name>")`
- Base class: LitElement with `static styles = css\`...\``
- All styles use `--sc-*` tokens — no raw hex, px spacing, or font-family
- Icons: import from `src/icons.ts` (Phosphor Regular). Never use emoji as UI icons.
- ARIA: every interactive component needs `role`, `aria-label` or `aria-labelledby`
- Focus: visible focus ring with `outline: 2px solid var(--sc-accent)` on `:focus-visible`
- Keyboard: Tab, Escape (overlays), Enter/Space (buttons), Arrow keys (lists/tabs)
- Events fire upward via `CustomEvent`; data flows down via `@property`
- Use slots for composition, not string props for content
- Animation: use `--sc-duration-*` and `--sc-ease-*` tokens. Respect `prefers-reduced-motion`.

## New Component Checklist

1. Create `src/components/sc-<name>.ts`
2. Add test in `components.test.ts` or `extra-components.test.ts`
3. Add entry in `catalog.html`
4. Add TypeScript types to `HTMLElementTagNameMap`
5. Add icon to `src/icons.ts` if needed (Phosphor Regular SVG paths)

## Commands

```bash
npm run check        # typecheck + lint + format + test
npm run test         # vitest
npm run test:e2e     # playwright
npm run build        # production build
```
