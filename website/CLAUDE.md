# website/ — Marketing Site

Astro static site at seaclaw.ai.

## Rules

- Font: Avenir via `var(--sc-font)`. Never import Google Fonts.
- Icons: inline Phosphor SVGs with `viewBox="0 0 256 256" fill="currentColor"`.
- Colors: `--sc-*` CSS custom properties only. No raw hex values.
- Spacing/radius: `--sc-space-*` and `--sc-radius-*` tokens only.
- Animation: `--sc-duration-*` and `--sc-ease-*` tokens. Respect `prefers-reduced-motion`.
- Accessibility: WCAG 2.1 AA minimum (4.5:1 text contrast, 3:1 UI contrast).

## Commands

```bash
npm run check        # astro check
npm run dev          # dev server
npm run build        # production build
npm run format:check # prettier
```
