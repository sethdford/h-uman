---
title: "Quiet Room: web modernization umbrella + design-language foundation"
date: 2026-09-26
status: proposed
---

# Quiet Room: Web Modernization Umbrella + Design-Language Foundation

> The assistant that's actually yours, and a website and dashboard that look like
> they believe it.

This document has two parts:

- **Part I (umbrella)** records the decisions made while brainstorming (2026-09-26) and
  splits the modernization into four sub-projects. Each one gets its own spec, plan and
  implementation cycle.
- **Part II (sub-project 1)** is the full spec for the first of those: the **Quiet
  Room design language**, meaning tokens, color, type and the build pipeline changes.

It supersedes the direction in `2026-03-22-sota-quiet-mastery-design.md` (deferred),
which it borrows from: that doc's "Apple editorial calm" is kept, and its Pixar-scale
motion ambitions are dropped.

---

## Part I: Umbrella

### I.1 Goal and audience

**Primary goal: design-award recognition** (Awwwards SOTD, CSS Design Awards, FWA). Juries score
Design, Usability, Creativity and **Content**, so craft alone is not enough; the
story has to be right too.

**Scope: the website (`website/`) and the web dashboard (`ui/`).** The native apps (iOS,
macOS, Android) are out of scope. Part II's token architecture is designed so this work
cannot change them.

### I.2 Decisions (made 2026-09-26)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Story: "actually yours."** Lead with the `docs/PRODUCT.md` thesis (a private, personal AI that learns you locally). Runtime specs become supporting proof for developers. | PRODUCT.md already says binary size is something "users don't feel". The current homepage sells the runtime, and a jury scores Content. |
| D2 | **Brand room: Evolve.** Keep the name, the logo mark, green as the anchor, Phosphor icons and Avenir for UI/body. Add a self-hosted display serif, re-author the web palette in OKLCH, and warm the tone. | Enough room for a distinctive identity without cutting ties to the native apps' tokens. |
| D3 | **Website frame: "Quiet Room"**: editorial, paper and ink, serif display. **Signature moment 1:** generic AI replies strike through and rewrite themselves in a human voice, driven by scroll. | The most on-thesis option. It shows the product's idea through the type itself. |
| D4 | **Loud moment: "Living Portrait"** in chapter 4. Visitors type and a portrait forms from their words. It runs entirely in the tab, with a live "0 bytes sent" counter. | Turns the creepiness risk into the site's strongest privacy argument, because the visitor experiences local-first. |
| D5 | **Homepage arc: 7 chapters** (below). | One claim per chapter. Calm, loud, calm rhythm. |
| D6 | **Dashboard: "Personal first"** IA (Today · People · You · Privacy, with the runtime views under **Workshop**) plus a **global ⌘K command bar**. | The website and app tell one story. The Privacy ring is chapter 5, shown with live data. |
| D7 | **Sequencing: foundation first** (approach A). Build 1 first, then 2, then 3; 4 runs in parallel on the C side. | The website is what gets judged, so it ships on a finished language. Sub-projects 2 and 3 never build against a moving foundation. |

**Homepage arc (D5):**

1. **Hero:** "The assistant that's actually yours." Scroll-driven rewrite (D3).
2. **Rented, not owned:** "Every other assistant is someone else's product, renting you access."
3. **It learns you:** voice, people, rhythm. Persona-as-architecture, explained in human terms.
4. **Made of you:** Living Portrait (D4). The one dark chapter.
5. **Where your life lives:** an honest boundary diagram (see I.4) with a "choose a local model" toggle.
6. **Already where you talk:** channels as lived moments, not a logo wall.
7. **Built like it means it:** developer proof (footprint, HuLa, terminal demo, dashboard peek, install), then the CTA.

Mapping from today's 10 sections: hero stats, demo, HuLa, terminal+dashboard and quality
all move into ch7; problem+contrast becomes ch2; device-spectrum becomes the ch5 diagram;
ecosystem becomes ch6; **Crystal Grid is cut**.

### I.3 Decomposition

| # | Sub-project | Depends on | Risk tier | Spec |
|---|---|---|---|---|
| **1** | **Quiet Room design language** (tokens, OKLCH, display type, build pipeline) | — | Medium | **Part II of this doc** |
| **2** | **Website rebuild** (the 7-chapter arc, both signature moments, award bar) | 1 | Low | next spec |
| **3** | **Dashboard, Personal first** (IA, ⌘K, Quiet reskin, regenerated visual baselines) | 1 | Medium | later spec |
| **4** | **Privacy ledger** (measured boundary counters in the gateway, a new RPC, a demo-gateway mock) | — | **High** (`src/gateway/`) | later spec |

Sub-project 3 renders the Privacy ring in an explicit **"not yet measured"** state until
sub-project 4 lands. It never shows a placeholder number
(`.claude/rules/no-number-without-a-measurement.md`). Sub-project 4 follows
`.claude/rules/feature-gate-requires-measurement.md` (OFF→SHADOW→LIVE) and
`.claude/rules/reports-success-does-nothing.md`: the ledger counts at the send path, not
by inference.

Execution for every sub-project: **sequential single-agent work, verified between steps.**
Parallel worktree isolation has failed silently in this environment three times
(`~/.claude/rules/worktree-isolation-lifecycle.md`).

### I.4 Content honesty (binding on sub-projects 2, 3 and 4)

The thesis line *"never sends your identity to a cloud"* is true of **identity, memory,
persona and history**. It is **not** true of a message sent to a cloud model: that message
crosses the boundary. Every privacy claim on the site and in the dashboard must say which
of the two it means. Concretely:

- Allowed: "Your memory, persona and history never leave your machine." "Choose a local model and nothing leaves at all."
- Not allowed: "Nothing leaves your device." "0 bytes of you sent" (when cloud model calls happened).

A privacy site that overclaims privacy is a bigger reputational risk for this product than
for most. Sub-project 2's spec must include a claim-by-claim review of the copy against the
architecture.

### I.5 Shared success criteria (award readiness, measured on sub-projects 2 and 3)

| Criterion | Target | How measured |
|---|---|---|
| Lighthouse (website) | Perf ≥ 95, A11y ≥ 98, BP ≥ 95, SEO ≥ 95 | existing `lighthouse` CI job + local run (already the bar in `website/CLAUDE.md`) |
| Core Web Vitals, mid-tier mobile | LCP ≤ 2.0 s, INP ≤ 200 ms, CLS ≤ 0.05 | Lighthouse mobile profile + CrUX after 28 days |
| Accessibility | axe-core clean, full keyboard path, WCAG 2.2 AA | existing axe CI job + manual keyboard pass |
| Reduced motion | Both signature moments have a static equivalent that carries the same meaning | `ui/e2e/reduced-motion.spec.ts` pattern, extended to the website |
| Third-party requests | **Zero** on page load, website and dashboard | Playwright request log. On-thesis: a privacy site should not ping third parties. |
| Honesty | Every privacy claim traced to the architecture (I.4) | review checklist in the sub-project 2 spec |
| Submission | Awwwards, CSSDA, FWA | `website/CLAUDE.md` checklist |

### I.6 Non-goals

- Native apps (iOS, macOS, Android): no visual change. Part II guarantees it mechanically.
- Docs content (`website/src/content/docs/`, 48 Starlight pages): only inherits the
  new tokens. No content rewrite.
- New product features beyond the Privacy ledger and the dashboard IA.
- WebGL/3D. Direction B ("Contained Light") was not chosen; the `3d` / `spatial` tokens stay unused.

---

## Part II: Sub-project 1, the Quiet Room design language

### II.1 Goal

Ship a **Quiet Room token layer** that the website and dashboard can opt into, with:

1. an OKLCH-authored, contrast-measured palette (paper and ink, green in defined roles), light and dark;
2. a self-hosted display serif alongside Avenir;
3. build-pipeline changes that make the layer **CSS-only** and make the native emitters
   **fail loudly** instead of silently emitting black;
4. a specimen on the website's existing `/design` page and the dashboard's `design-system` view.

**Sub-project 1 changes no production page's appearance.** Pages opt in during sub-projects 2 and 3.

### II.2 Token architecture

**A new orthogonal attribute, `data-brand="quiet"`.** It is independent of `data-theme`,
which already means light/dark (`build.ts` lines 919–1003). Using `data-theme="quiet"`
would collide with that. (This corrects the wording used during brainstorming.)

Selectors emitted to the **CSS outputs only** (`ui/src/styles/_tokens.css`,
`website/src/styles/_tokens.css`):

```css
/* Q = :is(:root[data-brand="quiet"], [data-brand="quiet"]), specificity (0,2,0) either way */
Q                                                  { /* typography (all modes) */ }
@media not (prefers-contrast: more) {
  Q                                                { /* light colors */ }
  @media (prefers-color-scheme: dark) {
    :is(:root:not([data-theme="light"])[data-brand="quiet"],
        :root:not([data-theme="light"]) [data-brand="quiet"]):not([data-theme="light"])
                                                   { /* dark colors */ }
  }
  [data-theme="dark"] [data-brand="quiet"]         { /* dark colors  — ancestor form */ }
  [data-theme="light"] [data-brand="quiet"]        { /* light colors — ancestor form */ }
  [data-theme="dark"][data-brand="quiet"]          { /* dark colors  — self form, last */ }
  [data-theme="light"][data-brand="quiet"]         { /* light colors — self form, last */ }
}
```

*Also corrected while planning:* the explicit-theme rules were first drafted as
`:is(self, ancestor)` pairs. But `:is()` scores as its most specific argument whichever
one matched, so self and ancestor tie at (0,2,0) and source order decides. A dark specimen
panel on a page the visitor had toggled to light would then have rendered light. The
fix is separate rules, with the self forms last. The plan's cascade matrix pins this case,
and it's shown to fail when the order is reversed.

**Why `:is()` (corrected 2026-09-26 while planning).** The first draft used bare
`:root[data-brand="quiet"]`, which can't match the `/design` page's specimen *container*
(II.6). Two cascade facts drive this shape:
- `:is()` takes the specificity of its most specific argument, so Q scores **(0,2,0)**
  even when it matches a nested container. It therefore beats the existing
  `@media (color-gamut: p3) { :root {…} }` accent overrides, which come *later* in
  `_tokens.css` at (0,1,0). That block applies on essentially every modern Mac.
- Explicit `data-theme` beats the system preference, and a nested container can carry its
  own `data-theme` so the specimen can show light and dark side by side.

- **It overrides existing semantic names; it adds no parallel vocabulary.** The quiet layer
  re-values `--hu-bg*`, `--hu-surface-container*`, `--hu-text*`, `--hu-accent*`,
  `--hu-on-accent`, `--hu-border*`, `--hu-focus-ring` and `--hu-link*`. Components already
  written against `--hu-*` pick it up with no code change, and `lint:tokens` keeps
  working unchanged.
- **Tokens it doesn't set are inherited** from the existing light/dark values
  (status colors, overlays, shadows). They are still subject to the contrast check in II.7.
- **High contrast wins.** The existing high-contrast values are emitted under
  `@media (prefers-contrast: more)` (`build.ts` ~line 960). The quiet color overrides are
  wrapped in `@media not (prefers-contrast: more)` so they never beat it. The quiet
  typography applies in both.
- **Source location:** a new `design-tokens/quiet.tokens.json` (W3C format like its
  siblings), carrying the **existing** extension key `"com.human.platform": "web"`. All 13
  current token files already declare `"com.human.platform": "all"`, but `build.ts` never
  reads it (verified 2026-09-26). Sub-project 1 makes the build honor it rather than
  inventing a new key. Since every existing file says `"all"`, honoring it changes no
  current output.

### II.3 Palette (measured 2026-09-26)

Authored in OKLCH and emitted as `oklch()` in CSS (Baseline in all target browsers). The
sRGB hex is shown for reference; all values are in gamut.

| Role | Light | Dark |
|---|---|---|
| `bg` (paper) | `oklch(97.5% 0.010 95)` #F9F7EF | `oklch(17% 0.012 150)` #0C110D |
| `bg-inset` / `surface-container` (paper-2) | `oklch(94.5% 0.012 95)` #EFEDE4 | `oklch(20.5% 0.014 150)` #131914 |
| `surface-container-high` (final review) | `oklch(93.5% 0.012 95)` #ECEAE1 | `oklch(24.5% 0.014 150)` #1C221D |
| `surface-container-highest` (final review) | `oklch(92.5% 0.012 95)` #E8E6DD | `oklch(27% 0.014 150)` #222823 |
| `bg-surface` / `bg-elevated` (card) | `oklch(99.5% 0.004 95)` #FEFDFA | `oklch(22.5% 0.014 150)` #171E18 |
| `bg-overlay` (final review) | `oklch(99.5% 0.004 95)` #FEFDFA | `oklch(24.5% 0.014 150)` #1C221D |
| `text` (ink) | `oklch(23% 0.020 150)` #162018 | `oklch(95% 0.010 120)` #EDF0E8 |
| `text-secondary` / `text-muted` | `oklch(50% 0.015 150)` #5D665F | `oklch(74% 0.012 120)` #AAACA4 |
| `text-tertiary` (final review) | `oklch(50% 0.012 95)` #65635C | `oklch(70% 0.010 120)` #9D9F99 |
| `text-faint` (final review) | `oklch(50.5% 0.012 95)` #67655D | `oklch(68% 0.010 120)` #979992 |
| `border` (line) | `oklch(89% 0.012 95)` #DDDBD2 | `oklch(32% 0.016 150)` #2D352F |
| `border-subtle` (decorative, final review) | `oklch(92% 0.010 95)` #E6E5DD | `oklch(28% 0.014 150)` #242B25 |
| `accent` / `accent-text` / `link` | `oklch(50% 0.130 135)` #40731A | `oklch(82% 0.160 135)` #95DB6C |
| `on-accent` | card #FEFDFA | paper #0C110D |
| `focus-ring` | `oklch(58% 0.140 135)` #538C2D | `oklch(80% 0.170 135)` #8CD55E |
| **new** `accent-brand` (decorative) | `oklch(71.5% 0.155 131)` #80B645 | `oklch(80% 0.170 135)` #8CD55E |

**Green has roles, not one value.** Brand `#7AB648` measures `oklch(71.2% 0.155 133.1)`.
On light paper it reaches only **2.25:1**, which fails WCAG's 3:1 minimum even for non-text UI. So:

- `accent-brand` (≈ brand green) is **decorative only** in light mode: illustration, large
  display glyphs, the portrait. It never carries information or text.
- Text, links, and filled buttons use `accent` at L = 50%.
- Rings and focus indicators use `focus-ring` at L = 58%.
- In dark mode the brighter greens clear every threshold (see below).

Measured contrast (WCAG 2.x relative luminance; script in II.7):

| Pair | Ratio | Need |
|---|---|---|
| ink on paper (light) | 15.63 | 4.5 |
| muted text on paper / paper-2 / card (light) | 5.54 / 5.07 / 5.87 | 4.5 |
| accent text on paper / paper-2 / card (light) | 5.33 / 4.88 / 5.64 | 4.5 |
| on-accent label on accent fill (light) | 5.64 | 4.5 |
| focus-ring on paper / paper-2 (light) | 3.80 / 3.47 | 3.0 |
| ~~brand green on paper (light)~~ | ~~2.25~~ | 3.0: **why the role split exists** |
| ink on paper (dark) | 16.52 | 4.5 |
| muted text on paper / card (dark) | 8.30 / 7.41 | 4.5 |
| accent text on paper / card (dark) | 11.45 / 10.23 | 4.5 |
| on-accent (paper) on accent fill (dark) | 10.74 | 4.5 |

**Inherited tokens that fail on paper (measured while planning, 2026-09-26).** The first
draft relied on the quiet layer *inheriting* status and link-state colors from the
existing themes. Measured against the new backgrounds, eight of them fail, so the quiet
layer overrides them too:

| Token | Existing value → min ratio | Quiet override → min ratio |
|---|---|---|
| light `text-faint` | #726c65 → 4.42 | ~~`oklch(52.5% 0.012 95)` → 4.59~~ `oklch(50.5% 0.012 95)` → 4.71 |
| light `link-active` | #7ab648 → **2.08** | `oklch(45% 0.125 135)` → 5.70 |
| light `accent-hover` (fill) | #5a9a30 → **2.93** | `oklch(45% 0.125 135)` → 6.06; on-accent label 7.01 |
| light `success` | #008000 → 4.38 | `oklch(50% 0.130 150)` → 4.53 |
| light `warning` | #ca8a04 → **2.50** | ~~`oklch(53% 0.105 65)` → 4.62~~ `oklch(51.5% 0.100 65)` → 4.63 (C 0.120 was out of sRGB gamut) |
| light `info` | #2563eb → 4.40 | `oklch(50% 0.160 258)` → 4.90 |
| light `text-tertiary` (final review) | #6b655e → 4.62 | `oklch(50% 0.012 95)` → 4.81 (owned so both modes carry the same key set) |
| light `link-visited` (final review) | #3a6a24 → 5.16 | `#3a6a24` → 5.16 (same value, owned for key-set symmetry) |
| light / dark `error` (final review) | #cc0000 / #f97066 | same values → 4.72 / 5.38 (owned so the P3 block cannot leak into it) |
| dark `text-faint` | #56504a → **2.14** | ~~`oklch(64% 0.010 120)` → 5.08~~ `oklch(68% 0.010 120)` → 5.22 |
| dark `text-tertiary` (final review) | #8a847e → **4.06** | `oklch(70% 0.010 120)` → 5.63 |
| dark `info` (final review) | #3b82f6 → **4.08** | `oklch(70% 0.140 258)` → 5.58 |
| dark `link-visited` (final review) | #5a9a30 → **4.35** | `oklch(76% 0.130 135)` → 7.27 |
| dark `accent-hover`, `link-active` | set for light/dark symmetry | `oklch(86% 0.150 135)` → 11.62 (`link-active` 10.23 on the eight roles) |

"Min ratio" in the override column is the worst case across the eight background roles in
`design-tokens/contrast-lib.ts` (`BG_ROLES`): bg, bg-inset, bg-surface, bg-elevated,
surface-container, surface-container-high, surface-container-highest and bg-overlay. The
"existing value" column keeps the planning-time three-background measurement, except for rows
marked *final review*, which are measured on the eight. The `accent-hover` 6.06 is its
planning-time text ratio; the checker measures it only as a fill under `on-accent`.
The checker's tightest pair is now **light `success` on `surface-container-highest`, 4.53:1**
(244 pairs, 0 failed, 0 out of gamut).

**Final-review correction, 2026-09-26.** The planning-time palette was measured on three
backgrounds, but the quiet layer also paints `--hu-surface-container-high`,
`--hu-surface-container-highest` and `--hu-bg-overlay`, which it neither re-valued nor
measured. Left inherited, those surfaces keep the default theme's greys under paper-and-ink
text. The layer now re-values all three (plus a decorative `border-subtle`) and the checker
measures every text and UI role on all eight. On the new darkest light surface
(`oklch(92.5% 0.012 95)`), the old light `text-faint` measured 4.32 and the old light
`warning` 4.35. On the new lightest dark surface (`oklch(27% 0.014 150)`), the old dark `info`
measured 4.08, and the inherited dark `text-tertiary` and `link-visited` measured 4.06 and
4.35. Those five were re-valued. `text-tertiary` and `link-visited` are now owned in both
modes because the build requires light and dark to override the same key set. `error` is
owned in both modes for a different reason: the base P3 block overrides an inherited
`--hu-error` whenever an explicit `data-theme` disagrees with the OS scheme (2.30–3.15:1).
Of the measured text roles, only `link-hover` is still inherited (5.16 light, 8.70 dark on
the eight backgrounds), and the checker re-measures it with the rest on every run.

These are the proposed starting values. The checker in II.7 is the authority: if a later
tweak fails it, the build fails.

### II.4 Typography

- **Display: Newsreader Variable** (Production Type, **OFL-1.1**), self-hosted via
  `@fontsource-variable/newsreader` 5.3.0. It has an optical-size axis (`opsz`), so the
  same family works for a 96 px hero and a 26 px dashboard greeting. Regular weight for
  display; italic for the emphasised words ("*actually yours.*").
- **UI / body: Avenir.** Unchanged (self-hosted woff2 in `website/public/fonts/avenir`).
- **Mono: Geist Mono.** Unchanged.
- New tokens: `--hu-font-display` and a `display-xl` size
  (`clamp(2.75rem, 1.5rem + 5vw, 6rem)`). Within the quiet scope, the existing
  `typeRole.display*` and `typeRole.headline*` roles resolve to `--hu-font-display`;
  title/body/label roles stay on Avenir.
- **Replace the Inter fallback from `fonts.gstatic.com`** in `ui/src/styles/theme.css`
  (currently a documented exception) with self-hosted `@fontsource-variable/inter`. A
  privacy product's dashboard should not send its users' IPs to Google on load, and it's
  required for I.5's zero-third-party criterion.
- `font-display: swap`; preload only the display face's Latin subset on pages whose LCP
  element uses it.

Why this face: a serif display is the core of the "Quiet Room" editorial feel. Newsreader
is open-licensed, variable with `opsz`, and designed for screen reading. Alternatives were
considered and not chosen: Fraunces (its "wonk" axis is too quirky for trust), Instrument Serif
(single weight, no `opsz`), and Source Serif 4 (reads too institutional).

### II.5 Build pipeline changes (`design-tokens/build.ts`)

1. **Fail loudly on unhandled color formats.** Today any value that isn't 6-digit `#hex`
   or `rgb[a]()` falls through to black, so an `oklch()` token would ship **black** to
   every native app with a green build. There are five **live** fall-through sites (line
   numbers as of `d7306a191`):
   - `hexToSwift` (~150);
   - `hexToKotlin` (~157);
   - `rgbaToKotlin` (~164);
   - `colorToKotlin` (~185);
   - `formatSwiftColor` (~1929), the actual Swift color emitter.

   Replace each with a thrown error that names the value. `generateCHeader` has no color
   conversion and needs no change. **Delete `colorToSwift` (~176):** it has zero callers
   (only its own definition matches), so it's dead code.
   **Measured 2026-09-26, two independent runs:** an instrumented build logged **zero**
   fall-through hits. A separate session classified all 163 color-ish tokens (117 hex via
   `Color(hex:)`, 28 `rgba()` via `Color(red:green:blue:opacity:)`, 18 non-colors
   filtered out), and deleting `colorToSwift` left the Swift, Kotlin, CSS and C outputs
   byte-identical. The throw therefore can't break today's build.
2. **Honor `com.human.platform`.** A file declaring `"web"` goes to the CSS emitters only.
   `quiet.tokens.json` never reaches the Swift, Kotlin, C, or JSON/TS docs outputs. An
   unknown platform value is also a thrown error.
3. **Emit the `data-brand="quiet"` selectors** from II.2, including the high-contrast
   exclusion.
4. **No OKLCH to native conversion in this sub-project** (YAGNI). The native redesign will
   need it; the fail-loud guard in step 1 makes sure that need can't be met by accident.

*Correction (2026-09-26):* an earlier draft called `colorToSwift`'s `rgba()`→black
placeholder a live native bug. It isn't: the function has no callers, and the real
emitter, `formatSwiftColor`, converts `rgba()` correctly. Deleting the function (step 1)
closes it with no output change.

### II.6 Consumption and specimen

- **Website:** `website/src/styles/global.css` adds `--hu-font-display` to the Tailwind
  `@theme` mapping. `src/pages/design.astro` gains a "Quiet Room" section rendered inside a
  `data-brand="quiet"` container: palette swatches with measured ratios, the type scale,
  buttons in their green roles, focus states, and light/dark side by side.
- **Dashboard:** `ui/src/views/design-system-view.ts` gains a quiet toggle that sets
  `data-brand="quiet"` on `<html>` for the session, rendering the existing component
  catalog in the new language. It's a practical preview of sub-project 3's reskin surface.
- **No production page sets `data-brand="quiet"`** in sub-project 1.

### II.7 Testing and acceptance

Each item is checked by running something, not by reading code.

| # | Check | Passes when |
|---|---|---|
| A1 | **Native outputs untouched** | `git diff origin/main -- apps/**/DesignTokens.swift apps/**/DesignTokens.kt include/human/design_tokens.h` is **empty** after `npm run build` |
| A2 | Drift | `bash design-tokens/check-drift.sh` passes (the existing CI `design-tokens` job) |
| A3 | **Fail-loud emitter** | A fixture token with value `color(display-p3 0.5 0.7 0.3)` makes `npm run build -- --outdir <tmp>` exit non-zero, naming the token. The same build without the fixture exits 0. |
| A4 | **Contrast checker** | New `design-tokens/check-contrast.ts` (`npm run check:contrast`, added to `npm run check` and the CI job) computes every text-role/background pair in the quiet layer, light and dark, **including inherited tokens**, and exits non-zero on any failure. **It must be shown to discriminate:** it fails on a fixture that sets `accent-text` to brand green (2.25:1) *for that reason*, and passes on the real palette. |
| A5 | CSS output | The generated `_tokens.css` contains the three quiet selectors and none of the quiet values leak into bare `:root` |
| A6 | Zero third-party font requests | A Playwright request log for website `/design` and the dashboard shows **no** request to `fonts.gstatic.com`, `fonts.googleapis.com`, or any non-origin host for fonts |
| A7 | Website | `npm run check` (astro check) and `npm run build` pass; Lighthouse on `/design` (median of 3 runs, since `ci-required-checks.md` records ±5% run-to-run noise) drops no category by more than 3 points against `origin/main` |
| A8 | Dashboard | `npm run check` passes (typecheck, lint, format, `lint:tokens`, vitest) |
| A9 | Visual | `/verify-ui` screenshots of the specimen in light, dark and `prefers-contrast: more` (emulated), judged against II.3 and II.4; in the last, the high-contrast colors must win |
| A10 | Rules updated | Project `CLAUDE.md` "Design System" and `website/CLAUDE.md` / `ui/CLAUDE.md` say "Avenir (UI/body) + Newsreader (display, web)". The Inter-from-gstatic exception note is removed. |

`/verify` returns `RESULT_verifier=PASS` on A1–A8 before the sub-project closes, followed by
one critic pass (at most two critic→fix rounds).

### II.8 Risks

| Risk | Mitigation |
|---|---|
| Serif-on-paper reads as "a publication," not a product | The dashboard keeps Avenir for all UI; serif only for display/headline roles. Revisit after the sub-project 2 hero exists, not in the abstract. |
| A third theme axis (`data-brand` × light/dark/high-contrast) multiplies the states to test | A4 enumerates all combinations mechanically; A9 screenshots each one |
| The fail-loud change surfaces an existing native token that has been silently black | That's the point. List it in the PR and fix or file it; don't re-silence it |
| Font payload hurts LCP | Latin subset, variable single file, preload only where it's the LCP element; A7 guards regressions |

### II.9 Resolved questions

- **Display face: Newsreader**, confirmed 2026-09-26 after a side-by-side of Newsreader,
  Fraunces (SOFT 30, WONK 0) and Instrument Serif. All three were set in the real hero,
  dashboard-greeting, dark chapter-4 and numeral lines, with the fonts' actual variable
  files loaded. Deciding factor: optical sizing keeps one family right from the 96 px hero
  down to the 26 px dashboard greeting. Instrument Serif's single master looked thin at
  greeting and numeral sizes.
