# website/ — Marketing Site

Astro static site at h-uman.ai.

## Rules

- Font: Avenir via `var(--hu-font)`. Never import Google Fonts.
- Icons: inline Phosphor SVGs with `viewBox="0 0 256 256" fill="currentColor"`.
- Colors: `--hu-*` CSS custom properties only. No raw hex values.
- Spacing/radius: `--hu-space-*` and `--hu-radius-*` tokens only.
- Animation: `--hu-duration-*` and `--hu-ease-*` tokens. Respect `prefers-reduced-motion`.
- Accessibility: WCAG 2.1 AA minimum (4.5:1 text contrast, 3:1 UI contrast).

## Commands

```bash
npm run check        # astro check
npm run dev          # dev server
npm run build        # production build
npm run format:check # prettier
```
