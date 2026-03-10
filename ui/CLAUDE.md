# ui/ — Web Dashboard

LitElement web components for the human dashboard.

## Rules

- Component tag: `hu-<name>`, registered with `@customElement("hu-<name>")`
- Base class: LitElement with `static styles = css\`...\``
- All styles use `--hu-*` tokens — no raw hex, px spacing, or font-family
- Icons: import from `src/icons.ts` (Phosphor Regular). Never use emoji as UI icons.
- ARIA: every interactive component needs `role`, `aria-label` or `aria-labelledby`
- Focus: visible focus ring with `outline: 2px solid var(--hu-accent)` on `:focus-visible`
- Keyboard: Tab, Escape (overlays), Enter/Space (buttons), Arrow keys (lists/tabs)
- Events fire upward via `CustomEvent`; data flows down via `@property`
- Use slots for composition, not string props for content
- Animation: use `--hu-duration-*` and `--hu-ease-*` tokens. Respect `prefers-reduced-motion`.

## New Component Checklist

1. Create `src/components/hu-<name>.ts`
2. Add test in `components.test.ts` or `extra-components.test.ts`
3. Add entry in `catalog.html`
4. Add TypeScript types to `HTMLElementTagNameMap`
5. Add icon to `src/icons.ts` if needed (Phosphor Regular SVG paths)

## Shadow DOM & E2E Selectors

LitElement renders inside Shadow DOM. Playwright selectors must account for this.

- Use `page.locator("hu-component-name")` then `.locator(":scope .inner-class")`
- Never use bare `.class-name` selectors — always scope to the host component first
- Prefer `data-testid` attributes for stable E2E anchors

## Demo Gateway Contract

`src/demo-gateway.ts` is the mock client for offline/demo UI development.

- When the real C gateway adds a method, also add the mock in `demo-gateway.ts`
- Mock response shapes must exactly mirror the real gateway's JSON structure
- Use realistic sample data — not empty arrays or minimal stubs
- Wire protocol: WebSocket JSON-RPC, methods use `noun.verb` (e.g., `chat.send`, `sessions.list`)

## Error & State Handling

- Every view must handle 3 states: loading (skeleton), populated, error
- Use `<hu-toast>` for transient notifications; inline error banners for persistent failures
- Never show raw error messages — map to human-friendly text
- Never render a blank screen

## Performance Budgets

- Lighthouse targets: Performance >90, Accessibility >95, Best Practices >95
- Animate only compositor properties (transform, opacity, filter) — no layout thrashing
- Lazy-load views not in the initial viewport

## Commands

```bash
npm run check        # typecheck + lint + format + test + lint:tokens
npm run test         # vitest
npm run test:e2e     # playwright
npm run build        # production build
npm run lint:tokens  # detect raw hex, rgba, hardcoded durations, raw breakpoints
```

## Token Lint

- `npm run lint:tokens` — flags design token drift in all `.ts` files
- `bash scripts/lint-raw-colors.sh --all` (from repo root) — flags raw hex colors
- Run both before any UI commit
- Approved alpha pattern: `color-mix(in srgb, var(--hu-token) XX%, transparent)` — not `rgba()`
