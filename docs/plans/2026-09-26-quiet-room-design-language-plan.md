---
title: "Quiet Room design language (sub-project 1): implementation plan"
date: 2026-09-26
status: proposed
---

# Quiet Room Design Language Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a web-only, opt-in `data-brand="quiet"` token layer: an OKLCH palette with measured contrast, the Newsreader display face, and fail-loud native color emitters. No native output and no production page changes appearance.

**Architecture:** `design-tokens/build.ts` is split into small, testable modules:
- `token-lib.ts` loads token files and routes them by platform;
- `color-emit-lib.ts` holds the native color emitters;
- `contrast-lib.ts` does OKLCH/WCAG measurement;
- `quiet-lib.ts` emits the quiet CSS.

A new `quiet.tokens.json` marked `"com.human.platform": "web"` is read into a separate token map that only the CSS emitter sees, so Swift, Kotlin, C and the docs JSON can't receive it. The emitted selectors use `:is()` so the layer works on `<html>` or on any container, and it beats the existing P3 block on specificity.

**Tech Stack:** TypeScript run via `tsx` 4.23 on Node 26, `node:test`, Playwright 1.63 (ui), Lit 3 (ui), Astro 7 + Tailwind 4 (website), `@fontsource-variable/newsreader` 5.3.0 (OFL-1.1).

**Spec:** `docs/plans/2026-09-26-quiet-room-design-language-design.md`, Part II. Read it alongside this plan: the spec says *why*, this plan says *how*.

## Global Constraints

- The Swift, Kotlin and C token outputs must stay **byte-identical**. Check: `git diff --stat origin/main -- apps/shared/HumanKit/Sources/HumanChatUI/DesignTokens.swift apps/android/app/src/main/java/ai/human/app/ui/DesignTokens.kt include/human/design_tokens.h` must be empty at every commit.
- `docs/tokens.json`, `docs/tokens.ts` and `docs/design-tokens-reference.json` must not gain any `quiet.*` key.
- No production page sets `data-brand="quiet"` in this sub-project. Only the `/design` specimen container and the dashboard's design-system toggle do.
- UI code: `--hu-*` tokens only. No raw hex, px spacing or font-family in `ui/src/**/*.ts`, and `npm run lint:tokens` must report 0 violations.
- Fonts: self-hosted only. No `fonts.googleapis.com` or `fonts.gstatic.com` anywhere in built output.
- Contrast floors: text roles ≥ **4.5:1**, non-text UI (`focus-ring`) ≥ **3:1**, `on-accent` on `accent` / `accent-hover` ≥ **4.5:1**. Both modes, measured against every background role.
- Every OKLCH value in `quiet.tokens.json` must be inside the sRGB gamut.
- Commits are conventional (`<type>(<scope>): …`) and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Work only in the worktree `/Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec` (branch `worktree-quiet-room-spec`). Use absolute paths or `git -C` in every Bash call, since `cd` does not persist between calls.
- Run file-editing work **sequentially**, one task at a time. Worktree isolation for parallel agents has failed silently in this environment (`~/.claude/rules/worktree-isolation-lifecycle.md`).

## Review Focus

These five failure modes are implied by the spec but not exercised by any single task's happy-path tests. Each one has a pinning test in the task named.

1. **An unresolved `{reference}` in `quiet.tokens.json`** would be emitted literally into CSS, and the browser would silently drop the declaration. Expected: the build fails naming the token. *Pinned in Task 4, `quiet-lib.test.ts` "throws on unresolved reference".*
2. **Light and dark override different sets of names**, so one mode silently inherits a value that was never measured against paper. Expected: the build fails listing the asymmetric names. *Pinned in Task 4, "throws on light/dark asymmetry".*
3. **`data-theme` and `data-brand` combinations, and P3 displays.** Explicit theme must beat system preference, a nested specimen container must work, and the quiet layer must beat the P3 block that is active on essentially every Mac. Expected: the right palette in every combination. *Pinned in Task 4, `ui/e2e/quiet-cascade.spec.ts` (a 9-case matrix, including a CDP-emulated P3 case with a precondition proving the P3 block is active).*
4. **The production CSS pipeline rewrites the selectors.** Vite / Lightning CSS / Tailwind could lower `:is()` or flatten the nested `@media`, changing specificity. Expected: the *built* dashboard still resolves quiet values. *Pinned in Task 6, `ui/e2e/quiet-preview.spec.ts`, which runs against `npm run preview`'s built bundle. For the website, a `dist/` assertion in Task 6.*
5. **A visitor without Avenir** (Windows, Linux) falls back to Inter. Today that fallback comes from Google's CDN. Expected: served from the same origin with the metric overrides kept. *Pinned in Task 5, `ui/e2e/fonts-first-party.spec.ts` (request log) plus the `dist/` scan.*

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `design-tokens/token-lib.ts` | **create** | `TOKEN_FILES`, `collectTokens`, `resolveRefs` (moved verbatim), `readTokenSources`, `platformOf`, `partitionByPlatform` |
| `design-tokens/color-emit-lib.ts` | **create** | Native color emitters (moved), fail-loud `UnsupportedColorError` |
| `design-tokens/contrast-lib.ts` | **create** | OKLCH/hex/rgb parsing, gamut, WCAG ratio, `checkQuietContrast`, `checkQuietGamut` |
| `design-tokens/quiet-lib.ts` | **create** | `generateQuietCSS` with selectors and validation |
| `design-tokens/check-contrast.ts` | **create** | CLI: loads real tokens, prints failures, exit 0 / 1 / 2 |
| `design-tokens/quiet.tokens.json` | **create** | The palette and type tokens (spec II.3 and II.4) |
| `design-tokens/tests/*.test.ts` | **create** | `node:test` suites for each lib |
| `design-tokens/build.ts` | modify | Import the libs; delete the moved and dead functions; platform routing in `main()`; append quiet CSS |
| `design-tokens/package.json` | modify | `test` and `check:contrast` scripts; `check` runs both |
| `.github/workflows/ci.yml` | modify | `design-tokens` job runs `npm test` and `npm run check:contrast` |
| `ui/src/styles/_tokens.css`, `website/src/styles/_tokens.css` | regenerate | Gain the quiet block |
| `ui/e2e/quiet-cascade.spec.ts` | **create** | Cascade matrix against the raw generated CSS |
| `ui/public/fonts/inter/*` | **create** | Vendored Inter latin woff2 and OFL license |
| `ui/src/styles/theme.css` | modify | Inter `src` points to the same origin; Newsreader import |
| `website/src/styles/global.css` | modify | Newsreader import; `--font-display` in `@theme` |
| `ui/e2e/fonts-first-party.spec.ts` | **create** | No third-party font requests (built app) |
| `website/scripts/check-first-party-fonts.mjs` | **create** | `dist/` scan for Google font hosts and remote `@font-face` sources |
| `ui/src/views/design-system-view.ts` | modify | Quiet Room preview switch |
| `ui/src/views/views.test.ts` | modify | Switch sets and clears `data-brand` on `<html>` |
| `ui/e2e/quiet-preview.spec.ts` | **create** | The built bundle resolves quiet values after the toggle |
| `website/src/pages/design.astro` | modify | "Quiet Room" specimen section |
| `CLAUDE.md`, `website/CLAUDE.md`, `ui/CLAUDE.md`, `design-tokens/CLAUDE.md` | modify | Typeface rule, platform key, new checks |

---

### Task 1: Token loading library and platform routing

**Files:**
- Create: `design-tokens/token-lib.ts`
- Create: `design-tokens/tests/token-lib.test.ts`
- Modify: `design-tokens/build.ts:13-80` (move out `TOKEN_FILES`, `TokenValue`, `TokenMap`, `collectTokens`, `resolveRefs`), `design-tokens/build.ts:318-333` (`main()` loading loop)
- Modify: `design-tokens/package.json` (`test` script)
- Modify: `.github/workflows/ci.yml` (`design-tokens` job, ~line 765)

**Interfaces:**
- Produces: `type TokenValue = string | number`; `type TokenMap = Record<string, TokenValue>`; `type Platform = "all" | "web"`; `interface TokenSource { file: string; data: Record<string, unknown> }`; `const TOKEN_FILES: readonly string[]`; `collectTokens(obj: unknown, prefix?: string): TokenMap`; `resolveRefs(tokens: TokenMap): TokenMap`; `readTokenSources(dir: string, files: readonly string[]): TokenSource[]`; `platformOf(src: TokenSource): Platform`; `partitionByPlatform(sources: TokenSource[]): { shared: TokenSource[]; web: TokenSource[] }`.

- [ ] **Step 1: Install dependencies and confirm the baseline is clean**

```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens ci
bash /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/check-drift.sh
```
Expected: `check-drift.sh` prints no `DRIFT:` lines and exits 0. If it drifts *before* any change, stop and report. The baseline is not clean and every later byte-identity claim would be meaningless.

- [ ] **Step 2: Write the failing tests**

Create `design-tokens/tests/token-lib.test.ts`:

```ts
import { test } from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import {
  TOKEN_FILES,
  collectTokens,
  partitionByPlatform,
  platformOf,
  readTokenSources,
  type TokenSource,
} from "../token-lib.js";

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("every real token file declares platform 'all'", () => {
  const sources = readTokenSources(DIR, TOKEN_FILES);
  assert.equal(sources.length, TOKEN_FILES.length);
  for (const s of sources) assert.equal(platformOf(s), "all", s.file);
});

test("platformOf throws when the key is missing, naming the file", () => {
  const src: TokenSource = { file: "x.tokens.json", data: { $extensions: {} } };
  assert.throws(() => platformOf(src), /x\.tokens\.json.*com\.human\.platform/);
});

test("platformOf throws on an unknown platform value", () => {
  const src: TokenSource = {
    file: "y.tokens.json",
    data: { $extensions: { "com.human.platform": "ios" } },
  };
  assert.throws(() => platformOf(src), /y\.tokens\.json.*"ios"/);
});

test("partitionByPlatform separates web from shared", () => {
  const a: TokenSource = { file: "a", data: { $extensions: { "com.human.platform": "all" } } };
  const w: TokenSource = { file: "w", data: { $extensions: { "com.human.platform": "web" } } };
  const { shared, web } = partitionByPlatform([a, w]);
  assert.deepEqual(shared.map((s) => s.file), ["a"]);
  assert.deepEqual(web.map((s) => s.file), ["w"]);
});

test("collectTokens skips $-prefixed keys and flattens $value leaves", () => {
  const map = collectTokens({
    $description: "ignored",
    light: { bg: { $value: "#fff", $type: "color" } },
  });
  assert.deepEqual(map, { "light.bg": "#fff" });
});
```

- [ ] **Step 3: Add the test script and run it to confirm it fails**

In `design-tokens/package.json` `"scripts"`, add `"test": "tsx --test tests/*.test.ts"`.

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: FAIL, with `Cannot find module '../token-lib.js'` (or `ERR_MODULE_NOT_FOUND`).

- [ ] **Step 4: Create `design-tokens/token-lib.ts`**

Move `TokenValue`, `TokenMap`, `TOKEN_FILES`, `collectTokens` and `resolveRefs` **verbatim** from `build.ts` (lines 18–80; keep their doc comments), export them, and add:

```ts
import * as fs from "fs";
import * as path from "path";

export type Platform = "all" | "web";

export interface TokenSource {
  file: string;
  data: Record<string, unknown>;
}

/** Read and parse each token file; a missing file is a hard error. */
export function readTokenSources(
  dir: string,
  files: readonly string[],
): TokenSource[] {
  return files.map((file) => {
    const p = path.join(dir, file);
    if (!fs.existsSync(p)) throw new Error(`Missing token file: ${p}`);
    return {
      file,
      data: JSON.parse(fs.readFileSync(p, "utf-8")) as Record<string, unknown>,
    };
  });
}

/**
 * Which emitters a token file feeds. "all" = every platform (CSS, Swift,
 * Kotlin, C, docs JSON); "web" = CSS only. Every file must declare it —
 * a missing or unknown value is refused rather than defaulted, because a
 * silent default is how web-only values would leak into native outputs.
 */
export function platformOf(src: TokenSource): Platform {
  const ext = (src.data.$extensions ?? {}) as Record<string, unknown>;
  const p = ext["com.human.platform"];
  if (p === undefined) {
    throw new Error(
      `${src.file}: missing $extensions["com.human.platform"] (expected "all" or "web")`,
    );
  }
  if (p !== "all" && p !== "web") {
    throw new Error(
      `${src.file}: unknown com.human.platform ${JSON.stringify(p)} (expected "all" or "web")`,
    );
  }
  return p;
}

export function partitionByPlatform(sources: TokenSource[]): {
  shared: TokenSource[];
  web: TokenSource[];
} {
  return {
    shared: sources.filter((s) => platformOf(s) === "all"),
    web: sources.filter((s) => platformOf(s) === "web"),
  };
}
```

- [ ] **Step 5: Rewire `build.ts`**

Delete the moved declarations from `build.ts` and add at the top:

```ts
import {
  TOKEN_FILES,
  collectTokens,
  partitionByPlatform,
  readTokenSources,
  resolveRefs,
  type TokenMap,
  type TokenValue,
} from "./token-lib.js";
```

Replace the loading loop at the start of `main()` (the old `for (const file of TOKEN_FILES) {…}` through `tokens = resolveRefs(tokens);`) with:

```ts
  const { shared, web } = partitionByPlatform(
    readTokenSources(TOKENS_DIR, TOKEN_FILES),
  );
  let tokens: TokenMap = {};
  let p3Colors: Record<string, string> = {};
  for (const { data } of shared) {
    tokens = { ...tokens, ...collectTokens(data) };
    const ext = data.$extensions as Record<string, unknown> | undefined;
    if (ext?.["human.p3Colors"]) {
      p3Colors = {
        ...p3Colors,
        ...(ext["human.p3Colors"] as Record<string, string>),
      };
    }
  }
  tokens = resolveRefs(tokens);
  // Web-only tokens never enter `tokens`, so the Swift/Kotlin/C/docs
  // emitters cannot see them. Consumed by the CSS emitter in Task 4.
  let webTokens: TokenMap = {};
  for (const { data } of web) webTokens = { ...webTokens, ...collectTokens(data) };
  void webTokens;
```

If `TokenValue` ends up unused in `build.ts` after the move, drop it from the import (`tsc`/tsx will not complain, but keep the import list honest).

- [ ] **Step 6: Run the tests and the drift gate**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: PASS, 5 tests.

Run: `bash /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/check-drift.sh`
Expected: exit 0, no `DRIFT:` lines. This refactor must not change any output.

- [ ] **Step 7: Wire tests into CI**

In `.github/workflows/ci.yml`, job `design-tokens` (~line 765), after `npm run build -- --outdir /tmp/dt-check` add a step:

```yaml
      - run: cd design-tokens && npm test
```

- [ ] **Step 8: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add design-tokens/token-lib.ts design-tokens/tests/token-lib.test.ts design-tokens/build.ts design-tokens/package.json .github/workflows/ci.yml
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "refactor(design-tokens): extract token loading and honor com.human.platform

All 13 token files already declared com.human.platform=\"all\" but build.ts
never read it. Loading moves to token-lib.ts, and web-only files are read
into a separate map the native emitters cannot see. Outputs unchanged
(check-drift clean).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Fail-loud native color emitters

**Files:**
- Create: `design-tokens/color-emit-lib.ts`
- Create: `design-tokens/tests/color-emit-lib.test.ts`
- Modify: `design-tokens/build.ts`: delete `hexToSwift` (~148), `hexToKotlin` (~155), `rgbaToKotlin` (~162), `colorToSwift` (~176, dead: zero callers), `colorToKotlin` (~182), `formatSwiftColor` (~1917); import from the lib.

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `class UnsupportedColorError extends Error`; `hexToSwift(hex: string): string`; `hexToKotlin(hex: string): string`; `rgbaToKotlin(rgba: string): string`; `colorToKotlin(val: string): string`; `formatSwiftColor(val: string): string`. Same names and return formats as today; the only behavior change is throw-instead-of-black.

- [ ] **Step 1: Write the failing tests**

Create `design-tokens/tests/color-emit-lib.test.ts`:

```ts
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  UnsupportedColorError,
  colorToKotlin,
  formatSwiftColor,
  hexToKotlin,
  hexToSwift,
} from "../color-emit-lib.js";

test("hex converts exactly as before", () => {
  assert.equal(hexToSwift("#7AB648"), "0x7AB648");
  assert.equal(hexToKotlin("#7ab648"), "0xFF7AB648");
  assert.equal(formatSwiftColor("#7ab648"), "Color(hex: 0x7AB648)");
});

test("rgba converts exactly as before", () => {
  assert.equal(
    formatSwiftColor("rgba(255, 255, 255, 0.08)"),
    "Color(red: 1, green: 1, blue: 1, opacity: 0.08)",
  );
  assert.equal(colorToKotlin("rgba(0, 0, 0, 0.5)"), "0x80000000");
});

test("rgb() without alpha is converted, not blackened", () => {
  assert.equal(colorToKotlin("rgb(122, 182, 72)"), "0xFF7AB648");
});

for (const bad of [
  "oklch(50% 0.13 135)",
  "color(display-p3 0.5 0.7 0.3)",
  "#fff",
  "#7AB648CC",
  "linear-gradient(red, blue)",
]) {
  test(`throws UnsupportedColorError for ${bad}`, () => {
    assert.throws(() => formatSwiftColor(bad), UnsupportedColorError);
    assert.throws(() => colorToKotlin(bad), UnsupportedColorError);
  });
}

test("the error names the offending value", () => {
  assert.throws(() => formatSwiftColor("oklch(50% 0.13 135)"), /oklch\(50% 0\.13 135\)/);
});
```

- [ ] **Step 2: Run to confirm failure**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: FAIL, `Cannot find module '../color-emit-lib.js'`.

- [ ] **Step 3: Create `design-tokens/color-emit-lib.ts`**

The regexes are copied from `build.ts` unchanged, so existing tokens convert byte-for-byte. The only differences are that the black fall-throughs become throws, and `colorToKotlin` also routes `rgb(` to `rgbaToKotlin`. No `rgb(` token exists today (measured 2026-09-26), so this changes no output.

```ts
/**
 * Native (Swift/Kotlin) color emitters. Anything these cannot represent is
 * refused: the previous fall-through returned black, so an unsupported value
 * (e.g. oklch) shipped black to every native app with a green build.
 */

export class UnsupportedColorError extends Error {
  constructor(fn: string, value: string) {
    super(
      `${fn}: unsupported color ${JSON.stringify(value)}. Native emitters accept ` +
        `#RRGGBB or rgb()/rgba(). Put web-only colors (oklch, display-p3) in a ` +
        `token file with "com.human.platform": "web".`,
    );
    this.name = "UnsupportedColorError";
  }
}

const HEX6 = /^#([0-9a-fA-F]{6})$/;
const RGBA = /rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/;

/** #rrggbb → 0xRRGGBB */
export function hexToSwift(hex: string): string {
  const m = hex.match(HEX6);
  if (!m) throw new UnsupportedColorError("hexToSwift", hex);
  return "0x" + m[1].toUpperCase();
}

/** #rrggbb → 0xFFRRGGBB */
export function hexToKotlin(hex: string): string {
  const m = hex.match(HEX6);
  if (!m) throw new UnsupportedColorError("hexToKotlin", hex);
  return "0xFF" + m[1].toUpperCase();
}

/** rgb[a](r,g,b[,a]) → 0xAARRGGBB */
export function rgbaToKotlin(rgba: string): string {
  const m = rgba.match(RGBA);
  if (!m) throw new UnsupportedColorError("rgbaToKotlin", rgba);
  const r = parseInt(m[1], 10);
  const g = parseInt(m[2], 10);
  const b = parseInt(m[3], 10);
  const a = m[4] ? Math.round(parseFloat(m[4]) * 255) : 255;
  const hex = (((a << 24) | (r << 16) | (g << 8) | b) >>> 0)
    .toString(16)
    .padStart(8, "0")
    .toUpperCase();
  return "0x" + hex;
}

export function colorToKotlin(val: string): string {
  if (val.startsWith("#")) return hexToKotlin(val);
  if (/^rgba?\(/.test(val)) return rgbaToKotlin(val);
  throw new UnsupportedColorError("colorToKotlin", val);
}

export function formatSwiftColor(val: string): string {
  if (val.startsWith("#")) return `Color(hex: ${hexToSwift(val)})`;
  const m = val.match(RGBA);
  if (m) {
    const r = Math.round((parseInt(m[1], 10) / 255) * 10000) / 10000;
    const g = Math.round((parseInt(m[2], 10) / 255) * 10000) / 10000;
    const b = Math.round((parseInt(m[3], 10) / 255) * 10000) / 10000;
    const a = m[4] ? Math.round(parseFloat(m[4]) * 10000) / 10000 : 1;
    return `Color(red: ${r}, green: ${g}, blue: ${b}, opacity: ${a})`;
  }
  throw new UnsupportedColorError("formatSwiftColor", val);
}
```

**Before trusting the copied regexes, read** `build.ts` `formatSwiftColor` (~1917) and `rgbaToKotlin` (~162) and confirm that `RGBA` above is character-identical to both. If they differ from each other, keep two constants rather than merging them, so no output changes.

- [ ] **Step 4: Rewire `build.ts`**

Delete the six functions listed under **Files** and add:

```ts
import {
  colorToKotlin,
  formatSwiftColor,
  hexToKotlin,
} from "./color-emit-lib.js";
```

(`hexToSwift` and `rgbaToKotlin` are only used inside the lib after the move. Verify with `grep -n 'hexToSwift\|rgbaToKotlin\|colorToSwift' /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/build.ts`, which must print nothing.)

Then confirm no black sentinel remains in the generator:
`grep -n '0x000000\|0xFF000000' /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/build.ts /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/color-emit-lib.ts` must print nothing.

- [ ] **Step 5: Run the tests and prove the outputs are byte-identical**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: PASS (5 from Task 1 + 12 here).

Run: `bash /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/check-drift.sh`
Expected: exit 0. If it fails with an `UnsupportedColorError`, a real token hits a path the planning measurement missed. **Do not re-silence it.** Report the token and value.

- [ ] **Step 6: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add design-tokens/color-emit-lib.ts design-tokens/tests/color-emit-lib.test.ts design-tokens/build.ts
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "fix(design-tokens): refuse unsupported colors instead of emitting black

Five native color emitters fell through to black for anything but #RRGGBB
or rgba(), so an oklch() token would have shipped black to iOS/macOS/
Android with a green build. They now throw UnsupportedColorError naming
the value. colorToSwift had no callers and is deleted. Measured: zero
current tokens reach the old fall-throughs; outputs are byte-identical.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: OKLCH and WCAG contrast library

**Files:**
- Create: `design-tokens/contrast-lib.ts`
- Create: `design-tokens/tests/contrast-lib.test.ts`

**Interfaces:**
- Consumes: `type TokenMap` from `token-lib.ts` (Task 1).
- Produces: `type Linear = readonly [number, number, number]`; `class UnmeasurableColorError extends Error`; `oklchToLinear(L: number, C: number, h: number): Linear`; `parseOpaqueColor(v: string): Linear`; `isInGamut(lin: Linear): boolean`; `contrastRatio(fg: string, bg: string): number`; `const BG_ROLES, TEXT_ROLES, UI_ROLES: readonly string[]`; `const FILL_PAIRS: readonly (readonly [string, string])[]`; `interface PairResult { mode: "light" | "dark"; fg: string; bg: string; need: number; ratio: number | null; ok: boolean; reason?: string }`; `checkQuietContrast(shared: TokenMap, web: TokenMap): PairResult[]`; `checkQuietGamut(web: TokenMap): string[]`.

- [ ] **Step 1: Write the failing tests**

Create `design-tokens/tests/contrast-lib.test.ts`. The expected ratios are the planning-time measurements in spec II.3.

```ts
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  BG_ROLES,
  FILL_PAIRS,
  TEXT_ROLES,
  UI_ROLES,
  UnmeasurableColorError,
  checkQuietContrast,
  checkQuietGamut,
  contrastRatio,
  isInGamut,
  oklchToLinear,
  parseOpaqueColor,
} from "../contrast-lib.js";

const near = (a: number, b: number, tol = 0.01) =>
  assert.ok(Math.abs(a - b) <= tol, `${a} not within ${tol} of ${b}`);

test("black on white is 21:1", () => near(contrastRatio("#000000", "#FFFFFF"), 21));

test("brand green on paper measures 2.25:1 (why the role split exists)", () =>
  near(contrastRatio("oklch(71.5% 0.155 131)", "oklch(97.5% 0.010 95)"), 2.25));

test("accent text on paper measures 5.33:1", () =>
  near(contrastRatio("oklch(50% 0.130 135)", "oklch(97.5% 0.010 95)"), 5.33));

test("rgb() and hex agree", () =>
  near(contrastRatio("rgb(122, 182, 72)", "#FFFFFF"), contrastRatio("#7AB648", "#FFFFFF"), 1e-9));

test("translucent and non-color values are unmeasurable, not guessed", () => {
  assert.throws(() => parseOpaqueColor("rgba(0, 0, 0, 0.5)"), UnmeasurableColorError);
  assert.throws(() => parseOpaqueColor("linear-gradient(red, blue)"), UnmeasurableColorError);
  assert.throws(() => parseOpaqueColor("color(display-p3 0.4 0.6 0.2)"), UnmeasurableColorError);
});

test("gamut: warning at C 0.120 is outside sRGB, at C 0.105 inside", () => {
  assert.equal(isInGamut(oklchToLinear(0.53, 0.12, 65)), false);
  assert.equal(isInGamut(oklchToLinear(0.53, 0.105, 65)), true);
});

/** A web map where every role is set to a known-passing value in both modes. */
function passingFixture(): Record<string, string> {
  const m: Record<string, string> = {};
  const set = (mode: string, names: readonly string[], v: string) => {
    for (const n of names) m[`quiet.${mode}.${n}`] = v;
  };
  set("light", BG_ROLES, "oklch(97.5% 0.010 95)");
  set("light", [...TEXT_ROLES, ...UI_ROLES, "accent", "accent-hover"], "oklch(23% 0.020 150)");
  set("light", ["on-accent"], "oklch(99.5% 0.004 95)");
  set("dark", BG_ROLES, "oklch(17% 0.012 150)");
  set("dark", [...TEXT_ROLES, ...UI_ROLES, "accent", "accent-hover"], "oklch(95% 0.010 120)");
  set("dark", ["on-accent"], "oklch(17% 0.012 150)");
  return m;
}

test("a passing palette produces zero failures (no false positives)", () => {
  const results = checkQuietContrast({}, passingFixture());
  const expected =
    2 * (TEXT_ROLES.length * BG_ROLES.length + UI_ROLES.length * BG_ROLES.length + FILL_PAIRS.length);
  assert.equal(results.length, expected);
  assert.deepEqual(results.filter((r) => !r.ok), []);
});

test("DISCRIMINATES: brand green as accent-text fails, and only for that reason", () => {
  const web = passingFixture();
  web["quiet.light.accent-text"] = "oklch(71.5% 0.155 131)";
  const failures = checkQuietContrast({}, web).filter((r) => !r.ok);
  assert.ok(failures.length > 0, "expected failures");
  for (const f of failures) {
    assert.equal(f.mode, "light");
    assert.equal(f.fg, "accent-text");
    assert.ok(f.ratio !== null && f.ratio < 3, `ratio ${f.ratio}`);
  }
});

test("inherited values are measured: a failing shared token is caught", () => {
  const web = passingFixture();
  delete web["quiet.light.warning"];
  const failures = checkQuietContrast({ "light.warning": "#ca8a04" }, web).filter((r) => !r.ok);
  assert.ok(failures.some((f) => f.fg === "warning" && f.ratio !== null && f.ratio < 3));
});

test("a role defined nowhere is a failure, not a skip", () => {
  const web = passingFixture();
  delete web["quiet.dark.text-faint"];
  const f = checkQuietContrast({}, web).filter((r) => !r.ok);
  assert.ok(f.some((r) => r.fg === "text-faint" && r.mode === "dark" && r.reason === "missing"));
});

test("gamut check names out-of-gamut web colors", () => {
  assert.deepEqual(checkQuietGamut({ "quiet.light.warning": "oklch(53% 0.120 65)" }), [
    "quiet.light.warning = oklch(53% 0.120 65) is outside the sRGB gamut",
  ]);
  assert.deepEqual(checkQuietGamut({ "quiet.light.warning": "oklch(53% 0.105 65)" }), []);
});
```

- [ ] **Step 2: Run to confirm failure**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: FAIL, `Cannot find module '../contrast-lib.js'`.

- [ ] **Step 3: Create `design-tokens/contrast-lib.ts`**

```ts
/**
 * Contrast measurement for the Quiet Room layer (spec II.3 / II.7-A4).
 * WCAG 2.x relative luminance; OKLCH → linear sRGB via Björn Ottosson's
 * published matrices. Anything that cannot be measured as an opaque sRGB
 * color is refused — a guessed ratio is worse than none.
 */
import type { TokenMap } from "./token-lib.js";

export type Linear = readonly [number, number, number];

export class UnmeasurableColorError extends Error {
  constructor(value: string, why: string) {
    super(`cannot measure ${JSON.stringify(value)}: ${why}`);
    this.name = "UnmeasurableColorError";
  }
}

const HEX6 = /^#([0-9a-fA-F]{6})$/;
const RGB =
  /^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([\d.]+)\s*)?\)$/;
const OKLCH = /^oklch\(\s*([\d.]+)(%?)\s+([\d.]+)\s+([\d.]+)\s*\)$/;

const toLinear = (c: number) =>
  c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;

export function oklchToLinear(L: number, C: number, h: number): Linear {
  const a = C * Math.cos((h * Math.PI) / 180);
  const b = C * Math.sin((h * Math.PI) / 180);
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ];
}

export function parseOpaqueColor(v: string): Linear {
  const s = v.trim();
  const hex = s.match(HEX6);
  if (hex) {
    const n = hex[1];
    return [0, 2, 4].map((i) => toLinear(parseInt(n.slice(i, i + 2), 16) / 255)) as unknown as Linear;
  }
  const rgb = s.match(RGB);
  if (rgb) {
    if (rgb[4] !== undefined && parseFloat(rgb[4]) < 1) {
      throw new UnmeasurableColorError(v, "translucent; contrast depends on what is behind it");
    }
    return [1, 2, 3].map((i) => toLinear(parseInt(rgb[i], 10) / 255)) as unknown as Linear;
  }
  const ok = s.match(OKLCH);
  if (ok) {
    const L = ok[2] === "%" ? parseFloat(ok[1]) / 100 : parseFloat(ok[1]);
    return oklchToLinear(L, parseFloat(ok[3]), parseFloat(ok[4]));
  }
  throw new UnmeasurableColorError(v, "not #RRGGBB, opaque rgb(), or oklch()");
}

export function isInGamut(lin: Linear, eps = 1e-4): boolean {
  return lin.every((c) => c >= -eps && c <= 1 + eps);
}

function luminance(lin: Linear): number {
  const [r, g, b] = lin.map((c) => Math.min(Math.max(c, 0), 1));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastRatio(fg: string, bg: string): number {
  const a = luminance(parseOpaqueColor(fg));
  const b = luminance(parseOpaqueColor(bg));
  return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
}

/** Backgrounds every text/UI role must read on (spec II.3). */
export const BG_ROLES = ["bg", "bg-inset", "bg-surface", "bg-elevated", "surface-container"] as const;
/** Roles rendered as text: ≥ 4.5:1 on every background. */
export const TEXT_ROLES = [
  "text", "text-secondary", "text-muted", "text-tertiary", "text-faint",
  "accent-text", "link", "link-hover", "link-active", "link-visited",
  "success", "warning", "error", "info",
] as const;
/** Non-text UI indicators: ≥ 3:1 on every background. */
export const UI_ROLES = ["focus-ring"] as const;
/** [label, fill] pairs: the label must read on the fill at ≥ 4.5:1. */
export const FILL_PAIRS = [
  ["on-accent", "accent"],
  ["on-accent", "accent-hover"],
] as const;

export interface PairResult {
  mode: "light" | "dark";
  fg: string;
  bg: string;
  need: number;
  ratio: number | null;
  ok: boolean;
  reason?: string;
}

/**
 * Effective value = the quiet override if present, else the inherited
 * semantic value for that mode. Inherited tokens are measured too: they
 * render on paper whether or not the quiet file mentions them.
 */
export function checkQuietContrast(shared: TokenMap, web: TokenMap): PairResult[] {
  const results: PairResult[] = [];
  for (const mode of ["light", "dark"] as const) {
    const eff = (n: string) => {
      const v = web[`quiet.${mode}.${n}`] ?? shared[`${mode}.${n}`];
      return v === undefined ? undefined : String(v);
    };
    const measure = (fg: string, bg: string, need: number) => {
      const f = eff(fg);
      const b = eff(bg);
      if (f === undefined || b === undefined) {
        results.push({ mode, fg, bg, need, ratio: null, ok: false, reason: "missing" });
        return;
      }
      try {
        const ratio = contrastRatio(f, b);
        results.push({ mode, fg, bg, need, ratio, ok: ratio >= need });
      } catch (e) {
        results.push({ mode, fg, bg, need, ratio: null, ok: false, reason: (e as Error).message });
      }
    };
    for (const bg of BG_ROLES) {
      for (const fg of TEXT_ROLES) measure(fg, bg, 4.5);
      for (const fg of UI_ROLES) measure(fg, bg, 3);
    }
    for (const [label, fill] of FILL_PAIRS) measure(label, fill, 4.5);
  }
  return results;
}

/** Every oklch() value in the web layer must be inside sRGB. */
export function checkQuietGamut(web: TokenMap): string[] {
  const errors: string[] = [];
  for (const [k, v] of Object.entries(web)) {
    const s = String(v);
    if (!s.startsWith("oklch(")) continue;
    if (!isInGamut(parseOpaqueColor(s))) errors.push(`${k} = ${s} is outside the sRGB gamut`);
  }
  return errors;
}
```

- [ ] **Step 4: Run the tests**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: PASS. If `brand green on paper` returns ≈2.27 instead of 2.25, the OKLCH matrix was mistyped. Compare it digit by digit against the code above; don't loosen the tolerance.

- [ ] **Step 5: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add design-tokens/contrast-lib.ts design-tokens/tests/contrast-lib.test.ts
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "feat(design-tokens): OKLCH + WCAG contrast measurement for web token layers

Measures effective (override-or-inherited) values for every text, UI and
fill pair in both modes; refuses translucent/non-color values rather than
guessing; flags out-of-gamut oklch. Discrimination pinned: brand green as
accent-text fails at 2.25:1 for exactly that reason.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The Quiet Room token layer, CSS emission, checker CLI and cascade matrix

**Files:**
- Create: `design-tokens/quiet.tokens.json`
- Create: `design-tokens/quiet-lib.ts`
- Create: `design-tokens/tests/quiet-lib.test.ts`
- Create: `design-tokens/check-contrast.ts`
- Create: `ui/e2e/quiet-cascade.spec.ts`
- Modify: `design-tokens/token-lib.ts` (`TOKEN_FILES` += `"quiet.tokens.json"`), `design-tokens/build.ts` (`main()`: append quiet CSS), `design-tokens/package.json` (`check:contrast`, `check`), `.github/workflows/ci.yml`
- Regenerate: `ui/src/styles/_tokens.css`, `website/src/styles/_tokens.css`

**Interfaces:**
- Consumes: `TokenMap`, `TOKEN_FILES`, `readTokenSources`, `partitionByPlatform`, `collectTokens`, `resolveRefs` (Task 1); `checkQuietContrast`, `checkQuietGamut`, `PairResult` (Task 3).
- Produces: `generateQuietCSS(web: TokenMap): string`; exported selector constants `QUIET_SCOPE`, `QUIET_DARK_AUTO`, `QUIET_DARK_ANCESTOR`, `QUIET_LIGHT_ANCESTOR`, `QUIET_DARK_SELF`, `QUIET_LIGHT_SELF`; the CSS custom properties `--hu-font-display`, `--hu-text-display-xl`, `--hu-type-{display-lg,display-md,headline-lg,headline-md,headline-sm}-family`, `--hu-accent-brand` (Tasks 6 and 7 consume these).

- [ ] **Step 1: Create `design-tokens/quiet.tokens.json`**

The values are exactly spec II.3 plus the planning-time overrides table.

```json
{
  "$schema": "https://design-tokens.github.io/community-group/format/",
  "$extensions": {
    "com.human.platform": "web",
    "com.human.version": "0.5.0"
  },
  "$description": "Quiet Room — web-only, opt-in (data-brand=\"quiet\"). Paper and ink, green by role. Values measured in docs/plans/2026-09-26-quiet-room-design-language-design.md §II.3; design-tokens/check-contrast.ts re-measures on every build.",
  "quiet": {
    "type": {
      "font-display": { "$value": "\"Newsreader Variable\", \"Iowan Old Style\", \"Palatino Linotype\", Georgia, serif", "$type": "fontFamily" },
      "text-display-xl": { "$value": "clamp(2.75rem, 1.5rem + 5vw, 6rem)", "$type": "dimension" },
      "type-display-lg-family": { "$value": "var(--hu-font-display)" },
      "type-display-md-family": { "$value": "var(--hu-font-display)" },
      "type-headline-lg-family": { "$value": "var(--hu-font-display)" },
      "type-headline-md-family": { "$value": "var(--hu-font-display)" },
      "type-headline-sm-family": { "$value": "var(--hu-font-display)" },
      "type-display-lg-weight": { "$value": "400" },
      "type-display-md-weight": { "$value": "400" },
      "type-display-lg-letter-spacing": { "$value": "-0.02em" }
    },
    "light": {
      "bg": { "$value": "oklch(97.5% 0.010 95)", "$type": "color" },
      "bg-inset": { "$value": "oklch(94.5% 0.012 95)", "$type": "color" },
      "surface-container": { "$value": "oklch(94.5% 0.012 95)", "$type": "color" },
      "bg-surface": { "$value": "oklch(99.5% 0.004 95)", "$type": "color" },
      "bg-elevated": { "$value": "oklch(99.5% 0.004 95)", "$type": "color" },
      "text": { "$value": "oklch(23% 0.020 150)", "$type": "color" },
      "text-secondary": { "$value": "oklch(50% 0.015 150)", "$type": "color" },
      "text-muted": { "$value": "oklch(50% 0.015 150)", "$type": "color" },
      "text-faint": { "$value": "oklch(52.5% 0.012 95)", "$type": "color" },
      "border": { "$value": "oklch(89% 0.012 95)", "$type": "color" },
      "accent": { "$value": "oklch(50% 0.130 135)", "$type": "color" },
      "accent-text": { "$value": "oklch(50% 0.130 135)", "$type": "color" },
      "accent-hover": { "$value": "oklch(45% 0.125 135)", "$type": "color" },
      "link": { "$value": "oklch(50% 0.130 135)", "$type": "color" },
      "link-active": { "$value": "oklch(45% 0.125 135)", "$type": "color" },
      "on-accent": { "$value": "oklch(99.5% 0.004 95)", "$type": "color" },
      "focus-ring": { "$value": "oklch(58% 0.140 135)", "$type": "color" },
      "accent-brand": { "$value": "oklch(71.5% 0.155 131)", "$type": "color", "$description": "Decorative only in light mode (2.25:1 on paper)" },
      "success": { "$value": "oklch(50% 0.130 150)", "$type": "color" },
      "warning": { "$value": "oklch(53% 0.105 65)", "$type": "color" },
      "info": { "$value": "oklch(50% 0.160 258)", "$type": "color" }
    },
    "dark": {
      "bg": { "$value": "oklch(17% 0.012 150)", "$type": "color" },
      "bg-inset": { "$value": "oklch(20.5% 0.014 150)", "$type": "color" },
      "surface-container": { "$value": "oklch(20.5% 0.014 150)", "$type": "color" },
      "bg-surface": { "$value": "oklch(22.5% 0.014 150)", "$type": "color" },
      "bg-elevated": { "$value": "oklch(22.5% 0.014 150)", "$type": "color" },
      "text": { "$value": "oklch(95% 0.010 120)", "$type": "color" },
      "text-secondary": { "$value": "oklch(74% 0.012 120)", "$type": "color" },
      "text-muted": { "$value": "oklch(74% 0.012 120)", "$type": "color" },
      "text-faint": { "$value": "oklch(64% 0.010 120)", "$type": "color" },
      "border": { "$value": "oklch(32% 0.016 150)", "$type": "color" },
      "accent": { "$value": "oklch(82% 0.160 135)", "$type": "color" },
      "accent-text": { "$value": "oklch(82% 0.160 135)", "$type": "color" },
      "accent-hover": { "$value": "oklch(86% 0.150 135)", "$type": "color" },
      "link": { "$value": "oklch(82% 0.160 135)", "$type": "color" },
      "link-active": { "$value": "oklch(86% 0.150 135)", "$type": "color" },
      "on-accent": { "$value": "oklch(17% 0.012 150)", "$type": "color" },
      "focus-ring": { "$value": "oklch(80% 0.170 135)", "$type": "color" },
      "accent-brand": { "$value": "oklch(80% 0.170 135)", "$type": "color" },
      "success": { "$value": "#10b981", "$type": "color" },
      "warning": { "$value": "#eab308", "$type": "color" },
      "info": { "$value": "#3b82f6", "$type": "color" }
    }
  }
}
```

(Dark `success`, `warning` and `info` restate today's dark values, measured at 6.72, 8.88 and 4.63. They're listed only so light and dark override the same names.)

- [ ] **Step 2: Write the failing emitter tests**

Create `design-tokens/tests/quiet-lib.test.ts`:

```ts
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  QUIET_DARK_ANCESTOR,
  QUIET_DARK_AUTO,
  QUIET_DARK_SELF,
  QUIET_LIGHT_ANCESTOR,
  QUIET_LIGHT_SELF,
  QUIET_SCOPE,
  generateQuietCSS,
} from "../quiet-lib.js";

const MIN = {
  "quiet.type.font-display": "\"Newsreader Variable\", serif",
  "quiet.light.bg": "oklch(97.5% 0.010 95)",
  "quiet.dark.bg": "oklch(17% 0.012 150)",
};

test("no web tokens → no CSS (build unaffected)", () => {
  assert.equal(generateQuietCSS({}), "");
});

test("emits every selector, the contrast guard, and both modes", () => {
  const css = generateQuietCSS(MIN);
  for (const sel of [
    QUIET_SCOPE, QUIET_DARK_AUTO, QUIET_DARK_ANCESTOR, QUIET_LIGHT_ANCESTOR, QUIET_DARK_SELF, QUIET_LIGHT_SELF,
  ]) {
    assert.ok(css.includes(sel), `missing selector ${sel}`);
  }
  assert.ok(css.includes("@media not (prefers-contrast: more)"));
  assert.ok(css.includes("@media (prefers-color-scheme: dark)"));
  assert.ok(css.includes("--hu-font-display: \"Newsreader Variable\", serif;"));
  assert.equal(css.match(/--hu-bg: oklch\(97\.5% 0\.010 95\);/g)?.length, 3); // scope + light ancestor + light self
  assert.equal(css.match(/--hu-bg: oklch\(17% 0\.012 150\);/g)?.length, 3); // auto + dark ancestor + dark self
});

test("self forms are emitted after ancestor forms (own data-theme wins)", () => {
  const css = generateQuietCSS(MIN);
  const lastAncestor = Math.max(css.indexOf(QUIET_DARK_ANCESTOR + " {"), css.indexOf(QUIET_LIGHT_ANCESTOR + " {"));
  const firstSelf = Math.min(css.indexOf(QUIET_DARK_SELF + " {"), css.indexOf(QUIET_LIGHT_SELF + " {"));
  assert.ok(lastAncestor > 0 && firstSelf > lastAncestor);
});

test("never declares on bare :root (no leakage into non-quiet pages)", () => {
  assert.doesNotMatch(generateQuietCSS(MIN), /^\s*:root\s*\{/m);
});

test("colors are guarded by prefers-contrast; typography is not", () => {
  const css = generateQuietCSS(MIN);
  const guard = css.indexOf("@media not (prefers-contrast: more)");
  assert.ok(css.indexOf("--hu-font-display") < guard, "type must precede the contrast guard");
  assert.ok(css.indexOf("--hu-bg") > guard, "colors must sit inside the contrast guard");
});

test("throws on unresolved reference", () => {
  assert.throws(
    () => generateQuietCSS({ ...MIN, "quiet.light.text": "{light.text}" }),
    /quiet\.light\.text.*unresolved reference/,
  );
});

test("throws on light/dark asymmetry, naming the token", () => {
  assert.throws(
    () => generateQuietCSS({ ...MIN, "quiet.light.warning": "oklch(53% 0.105 65)" }),
    /light-only: \[warning\]/,
  );
});

test("throws on a path outside quiet.{light,dark,type}", () => {
  assert.throws(() => generateQuietCSS({ ...MIN, "quiet.spacing.x": "1rem" }), /unexpected token path "quiet\.spacing\.x"/);
});
```

- [ ] **Step 3: Run to confirm failure**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: FAIL, `Cannot find module '../quiet-lib.js'`.

- [ ] **Step 4: Create `design-tokens/quiet-lib.ts`**

```ts
/**
 * Emits the Quiet Room layer (spec II.2). Opt in with data-brand="quiet" on
 * <html> or any container. :is() takes its most specific argument, so every
 * selector here scores ≥ (0,2,0) — above the later `@media (color-gamut: p3)
 * { :root {…} }` block at (0,1,0) that applies on essentially every Mac.
 */
import type { TokenMap } from "./token-lib.js";

export const QUIET_SCOPE =
  ':is(:root[data-brand="quiet"], [data-brand="quiet"])';
export const QUIET_DARK_AUTO =
  ':is(:root:not([data-theme="light"])[data-brand="quiet"], ' +
  ':root:not([data-theme="light"]) [data-brand="quiet"]):not([data-theme="light"])';
// Explicit themes. :is() takes its MOST specific argument whichever one
// matched, so "self" and "ancestor" forms must be separate rules: all four
// score (0,2,0), and the self forms are emitted LAST so an element's own
// data-theme beats an ancestor's (e.g. a dark specimen panel on a page the
// visitor toggled to light).
export const QUIET_DARK_ANCESTOR = '[data-theme="dark"] [data-brand="quiet"]';
export const QUIET_LIGHT_ANCESTOR = '[data-theme="light"] [data-brand="quiet"]';
export const QUIET_DARK_SELF = '[data-theme="dark"][data-brand="quiet"]';
export const QUIET_LIGHT_SELF = '[data-theme="light"][data-brand="quiet"]';

const PATH = /^quiet\.(light|dark|type)\.[a-z0-9-]+$/;

function group(web: TokenMap, prefix: string): Map<string, string> {
  const m = new Map<string, string>();
  for (const k of Object.keys(web).sort()) {
    if (k.startsWith(prefix)) m.set(k.slice(prefix.length), String(web[k]));
  }
  return m;
}

const decls = (m: Map<string, string>, indent: string) =>
  [...m].map(([n, v]) => `${indent}--hu-${n}: ${v};`);

export function generateQuietCSS(web: TokenMap): string {
  const keys = Object.keys(web);
  if (keys.length === 0) return "";
  for (const k of keys) {
    if (!PATH.test(k)) {
      throw new Error(
        `quiet.tokens.json: unexpected token path "${k}" (expected quiet.light.*, quiet.dark.* or quiet.type.*)`,
      );
    }
    const v = String(web[k]);
    if (v.includes("{")) {
      throw new Error(
        `quiet.tokens.json: ${k} = ${v} contains an unresolved reference; web-only tokens must be literal values`,
      );
    }
  }
  const light = group(web, "quiet.light.");
  const dark = group(web, "quiet.dark.");
  const type = group(web, "quiet.type.");
  const onlyLight = [...light.keys()].filter((n) => !dark.has(n));
  const onlyDark = [...dark.keys()].filter((n) => !light.has(n));
  if (onlyLight.length || onlyDark.length) {
    throw new Error(
      `quiet.tokens.json: light and dark must override the same names; ` +
        `light-only: [${onlyLight.join(", ")}], dark-only: [${onlyDark.join(", ")}]`,
    );
  }
  const out = [
    "/* Quiet Room — web-only layer (design-tokens/quiet.tokens.json).",
    '   Opt in with data-brand="quiet" on <html> or on any container. Put data-theme',
    "   on <html> or on the quiet element itself; data-theme on an intermediate",
    "   wrapper is not supported. */",
  ];
  if (type.size) out.push(`${QUIET_SCOPE} {`, ...decls(type, "  "), "}");
  if (light.size) {
    out.push(
      "@media not (prefers-contrast: more) {",
      `  ${QUIET_SCOPE} {`, ...decls(light, "    "), "  }",
      "  @media (prefers-color-scheme: dark) {",
      `    ${QUIET_DARK_AUTO} {`, ...decls(dark, "      "), "    }",
      "  }",
      `  ${QUIET_DARK_ANCESTOR} {`, ...decls(dark, "    "), "  }",
      `  ${QUIET_LIGHT_ANCESTOR} {`, ...decls(light, "    "), "  }",
      `  ${QUIET_DARK_SELF} {`, ...decls(dark, "    "), "  }",
      `  ${QUIET_LIGHT_SELF} {`, ...decls(light, "    "), "  }",
      "}",
    );
  }
  return out.join("\n");
}
```

- [ ] **Step 5: Run the emitter tests**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens test`
Expected: PASS (all suites).

- [ ] **Step 6: Wire it into the build and regenerate**

In `token-lib.ts`, append `"quiet.tokens.json"` to `TOKEN_FILES`.

In `build.ts`: import `generateQuietCSS` from `./quiet-lib.js`; delete the `void webTokens;` line from Task 1; change `const css = generateCSS(tokens, p3Colors);` to:

```ts
  const quietCss = generateQuietCSS(webTokens);
  const css = quietCss
    ? `${generateCSS(tokens, p3Colors)}\n\n${quietCss}\n`
    : generateCSS(tokens, p3Colors);
```

Update the Task 1 test `every real token file declares platform 'all'` so it now expects `"web"` for `quiet.tokens.json` and `"all"` for the other 13:

```ts
  for (const s of sources) {
    assert.equal(platformOf(s), s.file === "quiet.tokens.json" ? "web" : "all", s.file);
  }
```

Then regenerate:
```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens run build
```
Expected: `Wrote … _tokens.css` for ui and website, then `Done.`

- [ ] **Step 7: Prove native and docs outputs are untouched (A1)**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec diff --stat -- apps/shared/HumanKit/Sources/HumanChatUI/DesignTokens.swift apps/android/app/src/main/java/ai/human/app/ui/DesignTokens.kt include/human/design_tokens.h docs/tokens.json docs/tokens.ts
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec grep -c 'quiet' -- docs/tokens.json docs/tokens.ts docs/design-tokens-reference.json
```
Expected: the first prints nothing. The second prints nothing, or `:0` for each file. `docs/design-tokens-reference.json` may show a changed `generated` timestamp only. Check with `git diff docs/design-tokens-reference.json`. If it changed, `git checkout` it so the commit carries no timestamp-only churn.

- [ ] **Step 8: Add the checker CLI**

Create `design-tokens/check-contrast.ts`:

```ts
#!/usr/bin/env node
/**
 * Measures the Quiet Room layer. Exit 0 = every pair passes; 1 = a pair
 * fails or a color is out of gamut; 2 = nothing to measure (no web tokens) —
 * distinct from 0 so a missing layer can never read as "passed".
 */
import * as path from "path";
import { fileURLToPath } from "url";
import {
  TOKEN_FILES,
  collectTokens,
  partitionByPlatform,
  readTokenSources,
  resolveRefs,
  type TokenMap,
} from "./token-lib.js";
import { checkQuietContrast, checkQuietGamut } from "./contrast-lib.js";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const { shared, web } = partitionByPlatform(readTokenSources(DIR, TOKEN_FILES));
let sharedMap: TokenMap = {};
for (const { data } of shared) sharedMap = { ...sharedMap, ...collectTokens(data) };
sharedMap = resolveRefs(sharedMap);
let webMap: TokenMap = {};
for (const { data } of web) webMap = { ...webMap, ...collectTokens(data) };

if (Object.keys(webMap).length === 0) {
  console.error("check-contrast: no web-only tokens found — nothing measured");
  process.exit(2);
}

const results = checkQuietContrast(sharedMap, webMap);
const gamut = checkQuietGamut(webMap);
const failed = results.filter((r) => !r.ok);
for (const f of failed) {
  const got = f.ratio === null ? f.reason : `${f.ratio.toFixed(2)}:1`;
  console.error(`FAIL ${f.mode.padEnd(5)} ${f.fg} on ${f.bg}: ${got} (need ${f.need}:1)`);
}
for (const g of gamut) console.error(`FAIL gamut ${g}`);
const worst = results
  .filter((r) => r.ratio !== null)
  .reduce((a, b) => (a.ratio! - a.need < b.ratio! - b.need ? a : b));
console.log(
  `check-contrast: ${results.length} pairs measured, ${failed.length} failed, ` +
    `${gamut.length} out of gamut; tightest ${worst.mode} ${worst.fg} on ${worst.bg} ` +
    `${worst.ratio!.toFixed(2)}:1 (need ${worst.need})`,
);
process.exit(failed.length || gamut.length ? 1 : 0);
```

In `design-tokens/package.json`: add `"check:contrast": "tsx check-contrast.ts"` and change `"check"` to `"npm run build && npm test && npm run check:contrast"`.

In `.github/workflows/ci.yml` `design-tokens` job, after the `npm test` step from Task 1, add:

```yaml
      - run: cd design-tokens && npm run check:contrast
```

- [ ] **Step 9: Run the checker against the real tokens**

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens run check:contrast`
Expected: exit 0, and a summary line reading `check-contrast: 154 pairs measured, 0 failed, 0 out of gamut; tightest …`. (154 = 2 modes × (14 text + 1 UI) × 5 backgrounds + 2 × 2 fill pairs.) From the planning measurements, the tightest pair should be light `focus-ring` on `bg-inset`/`surface-container` at 3.47:1, need 3.

If any pair fails, **don't adjust the checker.** Report the pair and ratio and fix the value in `quiet.tokens.json`, keeping spec II.3 in sync.

- [ ] **Step 10: Write the cascade matrix (Review Focus #3)**

Create `ui/e2e/quiet-cascade.spec.ts`. It inlines the real generated CSS into a blank page, so it tests exactly what `build.ts` emits and needs no app code.

```ts
import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const TOKENS_CSS = readFileSync(
  fileURLToPath(new URL("../src/styles/_tokens.css", import.meta.url)),
  "utf-8",
);
const QUIET = JSON.parse(
  readFileSync(fileURLToPath(new URL("../../design-tokens/quiet.tokens.json", import.meta.url)), "utf-8"),
).quiet;
const LIGHT_BG: string = QUIET.light.bg.$value;
const DARK_BG: string = QUIET.dark.bg.$value;
const norm = (s: string) => s.trim().replace(/\s+/g, " ");

async function load(
  page: Page,
  htmlAttrs: string,
  body: string,
  media: { scheme: "light" | "dark"; p3?: boolean; contrastMore?: boolean },
) {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", {
    features: [
      { name: "prefers-color-scheme", value: media.scheme },
      { name: "color-gamut", value: media.p3 ? "p3" : "srgb" },
      { name: "prefers-contrast", value: media.contrastMore ? "more" : "no-preference" },
    ],
  });
  await page.setContent(
    `<!doctype html><html ${htmlAttrs}><head><style>${TOKENS_CSS}</style></head><body>${body}</body></html>`,
  );
}
const prop = (page: Page, sel: string, name: string) =>
  page.$eval(sel, (el, n) => getComputedStyle(el).getPropertyValue(n), name).then(norm);

test.describe("Quiet Room cascade", () => {
  test("root quiet follows system light", async ({ page }) => {
    await load(page, 'data-brand="quiet"', "", { scheme: "light" });
    expect(await prop(page, "html", "--hu-bg")).toBe(norm(LIGHT_BG));
  });

  test("root quiet follows system dark", async ({ page }) => {
    await load(page, 'data-brand="quiet"', "", { scheme: "dark" });
    expect(await prop(page, "html", "--hu-bg")).toBe(norm(DARK_BG));
  });

  test("explicit data-theme=dark beats system light", async ({ page }) => {
    await load(page, 'data-brand="quiet" data-theme="dark"', "", { scheme: "light" });
    expect(await prop(page, "html", "--hu-bg")).toBe(norm(DARK_BG));
  });

  test("explicit data-theme=light beats system dark", async ({ page }) => {
    await load(page, 'data-brand="quiet" data-theme="light"', "", { scheme: "dark" });
    expect(await prop(page, "html", "--hu-bg")).toBe(norm(LIGHT_BG));
  });

  test("nested quiet container works and does not leak to <html>", async ({ page }) => {
    await load(page, "", '<div id="q" data-brand="quiet"></div>', { scheme: "light" });
    expect(await prop(page, "#q", "--hu-bg")).toBe(norm(LIGHT_BG));
    expect(await prop(page, "html", "--hu-bg")).not.toBe(norm(LIGHT_BG));
  });

  test("an element's own data-theme beats an ancestor's (dark panel on a page toggled light)", async ({ page }) => {
    await load(page, 'data-theme="light"', '<div id="d" data-brand="quiet" data-theme="dark"></div>', {
      scheme: "light",
    });
    expect(await prop(page, "#d", "--hu-bg")).toBe(norm(DARK_BG));
  });

  test("container with its own data-theme renders side by side", async ({ page }) => {
    await load(
      page,
      "",
      '<div id="l" data-brand="quiet" data-theme="light"></div><div id="d" data-brand="quiet" data-theme="dark"></div>',
      { scheme: "dark" },
    );
    expect(await prop(page, "#l", "--hu-bg")).toBe(norm(LIGHT_BG));
    expect(await prop(page, "#d", "--hu-bg")).toBe(norm(DARK_BG));
  });

  test("quiet beats the P3 block (which is active on most Macs)", async ({ page }) => {
    // Precondition: prove the P3 block is live under emulation, or this test proves nothing.
    await load(page, "", "", { scheme: "light", p3: true });
    expect(await prop(page, "html", "--hu-accent")).toMatch(/^color\(display-p3/);
    await load(page, 'data-brand="quiet"', "", { scheme: "light", p3: true });
    expect(await prop(page, "html", "--hu-accent")).toBe(norm(QUIET.light.accent.$value));
    expect(await prop(page, "html", "--hu-link")).toBe(norm(QUIET.light.link.$value));
  });

  test("prefers-contrast: more wins over quiet colors; quiet type still applies", async ({ page }) => {
    const hcBlock = TOKENS_CSS.match(/@media \(prefers-contrast: more\)\s*\{\s*:root\s*\{([^}]*)\}/);
    const hcText = hcBlock?.[1].match(/--hu-text:\s*([^;]+);/)?.[1];
    expect(hcText, "precondition: the high-contrast block defines --hu-text").toBeTruthy();
    await load(page, 'data-brand="quiet"', "", { scheme: "light", contrastMore: true });
    expect(await prop(page, "html", "--hu-text")).toBe(norm(hcText!));
    expect(await prop(page, "html", "--hu-font-display")).toContain("Newsreader Variable");
  });
});
```

- [ ] **Step 11: Run the cascade matrix**

```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui ci
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui run build
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui exec -- playwright test e2e/quiet-cascade.spec.ts
```
(The config's `webServer` runs `npm run preview`, so the build must exist even though this spec uses `setContent`.)
Expected: 9 passed.

**Prove the matrix discriminates, twice.** Rebuild tokens after each temporary change.
- Change `QUIET_SCOPE` to `':root[data-brand="quiet"]'`. "nested quiet container works" must FAIL.
- Restore that, then emit the two *self* rules **before** the two *ancestor* rules. "an element's own data-theme beats an ancestor's" must FAIL.

Restore both, rebuild, and confirm 9 passed. Record the three summary lines in the commit message.

- [ ] **Step 12: Drift gate and commit**

Run: `bash /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/design-tokens/check-drift.sh`
Expected: exit 0.

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add design-tokens/quiet.tokens.json design-tokens/quiet-lib.ts design-tokens/tests/quiet-lib.test.ts design-tokens/check-contrast.ts design-tokens/token-lib.ts design-tokens/tests/token-lib.test.ts design-tokens/build.ts design-tokens/package.json .github/workflows/ci.yml ui/src/styles/_tokens.css website/src/styles/_tokens.css ui/e2e/quiet-cascade.spec.ts
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "feat(design-tokens): Quiet Room web-only token layer

quiet.tokens.json (platform=web) emits CSS only, under
:is(:root[data-brand=quiet], [data-brand=quiet]) so it works on <html> or a
container and out-specifies the P3 :root block. check-contrast measures
154 pairs in both modes incl. inherited tokens: 0 failed. Cascade matrix
9/9; it fails the container case with the scope narrowed to :root, and the
own-theme case with self rules emitted before ancestor rules.
Native and docs outputs unchanged.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Self-hosted fonts (Newsreader, first-party Inter)

**Files:**
- Modify: `website/package.json`, `ui/package.json` (add `@fontsource-variable/newsreader@5.3.0`)
- Modify: `website/src/styles/global.css:1-21` (imports; `--font-display` in `@theme`)
- Modify: `ui/src/styles/theme.css:1-25` (Newsreader imports; Inter `src` → same origin; comment)
- Create: `ui/public/fonts/inter/inter-latin-wght-normal.woff2`, `ui/public/fonts/inter/LICENSE`
- Create: `ui/e2e/fonts-first-party.spec.ts`
- Create: `website/scripts/check-first-party-fonts.mjs`; modify `website/package.json` (`check:fonts`)

**Interfaces:**
- Consumes: `--hu-font-display` (Task 4).
- Produces: the font family `"Newsreader Variable"` loadable in both apps; Tailwind utility `font-display` on the website.

- [ ] **Step 1: Write the failing first-party test (ui)**

Create `ui/e2e/fonts-first-party.spec.ts`:

```ts
import { test, expect } from "@playwright/test";

const GOOGLE = /fonts\.(googleapis|gstatic)\.com/;

// A second test ("design-system view loads fonts only from its own origin")
// is appended in Task 6, once the specimen card actually uses Newsreader.

test("the Inter fallback is declared from the same origin", async ({ page }) => {
  await page.goto("/?demo");
  const srcs = await page.evaluate(() =>
    [...document.styleSheets].flatMap((s) => {
      try {
        return [...s.cssRules]
          .filter((r) => r instanceof CSSFontFaceRule && r.style.getPropertyValue("font-family").includes("Inter"))
          .map((r) => (r as CSSFontFaceRule).style.getPropertyValue("src"));
      } catch {
        return [];
      }
    }),
  );
  expect(srcs.length).toBeGreaterThan(0);
  for (const s of srcs) expect(s).not.toMatch(GOOGLE);
});
```

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui exec -- playwright test e2e/fonts-first-party.spec.ts`
Expected: FAIL, because the Inter `src` still names `fonts.gstatic.com`.

- [ ] **Step 2: Install Newsreader and confirm the family name**

```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website install @fontsource-variable/newsreader@5.3.0
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui install @fontsource-variable/newsreader@5.3.0
grep -h "font-family" /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui/node_modules/@fontsource-variable/newsreader/opsz.css | sort -u
```
Expected: `font-family: 'Newsreader Variable';`. If the family name differs, change `quiet.type.font-display` in `quiet.tokens.json` to match, rebuild tokens, and rerun Task 4's cascade matrix.

- [ ] **Step 3: Vendor Inter (latin, variable weight) from the npm package**

```bash
npm pack @fontsource-variable/inter@5.3.0 --pack-destination /private/tmp/claude-501/-Users-sethford-Library-Application-Support-Claude-scratch-workspaces-045d9450-e99d-4e9b-8930-9004c3e4921c-06f6e12c-8ce9-4af0-9d17-e68ca0243c39-scratch-2026-09-26-4b80d4/d84673da-5b38-49ee-bdde-47ec36304fcf/scratchpad
mkdir -p /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui/public/fonts/inter
tar -xzf /private/tmp/claude-501/-Users-sethford-Library-Application-Support-Claude-scratch-workspaces-045d9450-e99d-4e9b-8930-9004c3e4921c-06f6e12c-8ce9-4af0-9d17-e68ca0243c39-scratch-2026-09-26-4b80d4/d84673da-5b38-49ee-bdde-47ec36304fcf/scratchpad/fontsource-variable-inter-5.3.0.tgz -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui/public/fonts/inter --strip-components=2 package/files/inter-latin-wght-normal.woff2
tar -xzf /private/tmp/claude-501/-Users-sethford-Library-Application-Support-Claude-scratch-workspaces-045d9450-e99d-4e9b-8930-9004c3e4921c-06f6e12c-8ce9-4af0-9d17-e68ca0243c39-scratch-2026-09-26-4b80d4/d84673da-5b38-49ee-bdde-47ec36304fcf/scratchpad/fontsource-variable-inter-5.3.0.tgz -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui/public/fonts/inter --strip-components=1 package/LICENSE
ls -la /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui/public/fonts/inter
```
Expected: `inter-latin-wght-normal.woff2` (tens of KB) and `LICENSE` (OFL-1.1). If the scratchpad path doesn't exist in the executor's environment, use any temporary directory. Only the two extracted files are committed.

- [ ] **Step 4: Edit `ui/src/styles/theme.css`**

After the four existing `@import` lines, add:

```css
@import "@fontsource-variable/newsreader/opsz.css";
@import "@fontsource-variable/newsreader/opsz-italic.css";
```

In the Inter `@font-face`, replace the comment and `src` (keep every metric override and the `unicode-range` exactly):

```css
/* Inter: cross-platform fallback when Avenir is unavailable (Windows, Linux).
   Only loads if Avenir isn't found locally — browsers skip unused @font-face.
   Metric overrides approximate Avenir's geometry to minimize CLS during swap.
   Self-hosted (from @fontsource-variable/inter 5.3.0, OFL-1.1): a privacy
   product must not send its users' IPs to a third-party font CDN. */
@font-face {
  font-family: "Inter";
  src: url("/fonts/inter/inter-latin-wght-normal.woff2") format("woff2");
```

- [ ] **Step 5: Edit `website/src/styles/global.css`**

After `@import "@fontsource-variable/geist-mono";` add:

```css
@import "@fontsource-variable/newsreader/opsz.css";
@import "@fontsource-variable/newsreader/opsz-italic.css";
```

In `@theme`, after `--font-mono: var(--hu-font-mono);` add:

```css
  --font-display: var(--hu-font-display, var(--hu-font));
```

- [ ] **Step 6: Add the website `dist/` check**

Create `website/scripts/check-first-party-fonts.mjs`:

```js
#!/usr/bin/env node
// Fails if built output references Google font hosts or declares a remote
// @font-face source. Run after `npm run build`.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

const DIST = new URL("../dist/", import.meta.url).pathname;
const GOOGLE = /fonts\.(googleapis|gstatic)\.com/;
const REMOTE_FACE = /@font-face\s*{[^}]*url\(\s*["']?https?:\/\//;

function* walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) yield* walk(p);
    else if (/\.(css|html|js)$/.test(name)) yield p;
  }
}

let scanned = 0;
const problems = [];
for (const file of walk(DIST)) {
  scanned++;
  const text = readFileSync(file, "utf-8");
  if (GOOGLE.test(text)) problems.push(`${file}: references a Google font host`);
  if (file.endsWith(".css") && REMOTE_FACE.test(text)) problems.push(`${file}: remote @font-face src`);
}
if (scanned === 0) {
  console.error("check-first-party-fonts: dist/ is empty — run npm run build first");
  process.exit(2);
}
for (const p of problems) console.error(p);
console.log(`check-first-party-fonts: ${scanned} files scanned, ${problems.length} problems`);
process.exit(problems.length ? 1 : 0);
```

In `website/package.json` scripts, add `"check:fonts": "node scripts/check-first-party-fonts.mjs"`.

- [ ] **Step 7: Build both apps and run the checks**

```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website ci
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website run build
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website run check:fonts
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui run build
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui exec -- playwright test e2e/fonts-first-party.spec.ts
```
Expected: `check:fonts` prints `0 problems` and exits 0. The Playwright spec shows 1 passed.

**Prove the check discriminates:** temporarily re-add `@import url("https://fonts.googleapis.com/css2?family=Inter");` at the top of `global.css`, rebuild, and run `check:fonts`. It must exit 1 naming a Google host. Then revert and rebuild.

- [ ] **Step 8: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add website/package.json website/package-lock.json website/src/styles/global.css website/scripts/check-first-party-fonts.mjs ui/package.json ui/package-lock.json ui/src/styles/theme.css ui/public/fonts/inter ui/e2e/fonts-first-party.spec.ts
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "feat(fonts): self-host Newsreader display face and the Inter fallback

Newsreader Variable (opsz, OFL-1.1) via fontsource on website and ui. The
dashboard's Inter fallback was fetched from fonts.gstatic.com, sending every
Windows/Linux user's IP to Google; it is now served from /fonts/inter with
the same metric overrides. dist scan and request-log specs pin both.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Specimens (website `/design`, dashboard design-system toggle)

**Files:**
- Modify: `ui/src/views/design-system-view.ts` (import `hu-switch`; `@state _quiet`; switch in the hero; a "Quiet Room" foundations card)
- Modify: `ui/src/views/views.test.ts` (toggle test)
- Create: `ui/e2e/quiet-preview.spec.ts`
- Modify: `website/src/pages/design.astro` (a nav link and a `<section id="quiet-room">` after `#principles`)

**Interfaces:**
- Consumes: `--hu-font-display`, `--hu-text-display-xl`, `--hu-accent-brand`, the quiet palette (Task 4); `hu-switch` (existing: `checked`, `label`, event `hu-change` with `detail.checked`).
- Produces: nothing consumed later.

- [ ] **Step 1: Write the failing unit test (ui)**

Append to `ui/src/views/views.test.ts`:

```ts
describe("hu-design-system-view quiet preview", () => {
  it("sets and clears data-brand on <html>", async () => {
    await import("./design-system-view.js");
    delete document.documentElement.dataset.brand;
    const el = document.createElement("hu-design-system-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const sw = el.shadowRoot!.querySelector('hu-switch[data-testid="quiet-toggle"]') as HTMLElement & {
      checked: boolean;
    };
    expect(sw).toBeTruthy();
    expect(sw.checked).toBe(false);
    sw.dispatchEvent(new CustomEvent("hu-change", { detail: { checked: true }, bubbles: true, composed: true }));
    expect(document.documentElement.dataset.brand).toBe("quiet");
    sw.dispatchEvent(new CustomEvent("hu-change", { detail: { checked: false }, bubbles: true, composed: true }));
    expect(document.documentElement.dataset.brand).toBeUndefined();
    el.remove();
  });
});
```

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui test -- src/views/views.test.ts`
Expected: FAIL, because `sw` is null.

- [ ] **Step 2: Implement the toggle**

In `ui/src/views/design-system-view.ts`:
- Change the decorator import to `import { customElement, state } from "lit/decorators.js";`.
- Add `import "../components/hu-switch.js";`.
- Inside the class:

```ts
  /** Mirrors <html data-brand="quiet">; persists for the session so other views preview it too. */
  @state() private _quiet = document.documentElement.dataset.brand === "quiet";

  private _onQuietChange(e: CustomEvent<{ checked: boolean }>) {
    this._quiet = e.detail.checked;
    if (this._quiet) document.documentElement.dataset.brand = "quiet";
    else delete document.documentElement.dataset.brand;
  }
```

- Add styles:

```css
    .quiet-toggle {
      margin-top: var(--hu-space-md);
    }

    .quiet-display {
      font-family: var(--hu-type-display-lg-family, var(--hu-font));
      font-size: var(--hu-text-display-xl, var(--hu-text-3xl));
      font-weight: var(--hu-type-display-lg-weight);
      letter-spacing: var(--hu-type-display-lg-letter-spacing);
      line-height: 1;
      margin: 0 0 var(--hu-space-sm);
    }

    .quiet-display em {
      color: var(--hu-accent-text);
    }
```

- In `render()`, inside `<hu-page-hero>` after the `<p class="meta">…</p>`, add:

```ts
        <hu-switch
          class="quiet-toggle"
          data-testid="quiet-toggle"
          label="Quiet Room preview"
          .checked=${this._quiet}
          @hu-change=${this._onQuietChange}
        ></hu-switch>
```

- In `.foundations-grid`, add as the first `<hu-card>`:

```ts
          <hu-card>
            <h3 class="card-title">Quiet Room display</h3>
            <p class="quiet-display">Actually <em>yours.</em></p>
            <p class="type-body">
              Newsreader with optical sizing when the preview is on; Avenir otherwise.
              <code class="path">--hu-font-display</code>, <code class="path">--hu-text-display-xl</code>.
            </p>
          </hu-card>
```

Run: `npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui test -- src/views/views.test.ts`
Expected: PASS.

- [ ] **Step 3: Write the built-bundle check (Review Focus #4)**

Create `ui/e2e/quiet-preview.spec.ts`:

```ts
import { test, expect } from "@playwright/test";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const QUIET = JSON.parse(
  readFileSync(fileURLToPath(new URL("../../design-tokens/quiet.tokens.json", import.meta.url)), "utf-8"),
).quiet;
const norm = (s: string) => s.trim().replace(/\s+/g, " ");

test("the BUILT bundle resolves quiet tokens after the toggle", async ({ page }) => {
  await page.emulateMedia({ colorScheme: "light" });
  await page.goto("/?demo#design-system");
  const view = page.locator("hu-design-system-view");
  await view.waitFor();
  const bg = () =>
    page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--hu-bg"));
  expect(norm(await bg())).not.toBe(norm(QUIET.light.bg.$value));
  await view.locator('hu-switch[data-testid="quiet-toggle"]').click();
  await expect.poll(async () => norm(await bg())).toBe(norm(QUIET.light.bg.$value));
  const family = await view
    .locator(".quiet-display")
    .evaluate((el) => getComputedStyle(el).fontFamily);
  expect(family).toContain("Newsreader Variable");
});
```

Also append to `ui/e2e/fonts-first-party.spec.ts` (created in Task 5). It is only meaningful now: the Quiet Room card is what makes the dashboard request Newsreader. Newsreader is never installed locally, so the request cannot be skipped the way an Avenir fallback can.

```ts
test("design-system view loads fonts only from its own origin", async ({ page, baseURL }) => {
  const origin = new URL(baseURL!).origin;
  const fontRequests: string[] = [];
  page.on("request", (r) => {
    if (r.resourceType() === "font" || GOOGLE.test(r.url())) fontRequests.push(r.url());
  });
  await page.goto("/?demo#design-system");
  const view = page.locator("hu-design-system-view");
  await view.waitFor();
  await view.locator('hu-switch[data-testid="quiet-toggle"]').click();
  await page.evaluate(() => document.fonts.ready);
  await page.waitForLoadState("networkidle");
  expect(
    fontRequests.some((u) => /newsreader/i.test(u)),
    `expected a Newsreader request, got: ${fontRequests.join(", ") || "none"}`,
  ).toBe(true);
  for (const u of fontRequests) expect(new URL(u).origin, u).toBe(origin);
});
```

- [ ] **Step 4: Website specimen**

In `website/src/pages/design.astro`:
- In the section nav (the `<a href="#dataviz">`… list, ~line 150), add `<a href="#quiet-room">Quiet Room</a>` as the first link.
- Insert immediately after the closing `</section>` of `#principles`:

```astro
      {/* ═══ 2b. QUIET ROOM (opt-in web layer; spec docs/plans/2026-09-26-quiet-room-design-language-design.md) ═══ */}
      <section id="quiet-room" class="py-24 md:py-32 px-6 cv-defer">
        <div class="max-w-5xl mx-auto">
          <h2 class="text-3xl font-bold mb-4">Quiet Room</h2>
          <p class="quiet-lede">
            The web-only editorial layer. Opt in with <code>data-brand="quiet"</code>. Light and dark
            are shown side by side; every text pair below is measured by
            <code>design-tokens/check-contrast.ts</code> on each build.
          </p>
          <div class="quiet-pair">
            {["light", "dark"].map((mode) => (
              <div class="quiet-panel" data-brand="quiet" data-theme={mode}>
                <p class="quiet-eyebrow">Personal AI, on your hardware · {mode}</p>
                <p class="quiet-hero">The assistant that's <em>actually yours.</em></p>
                <p class="quiet-greeting">Good evening. Three people are waiting on you.</p>
                <p class="quiet-body">Your memory, persona and history never leave your machine.</p>
                <div class="quiet-actions">
                  <a class="quiet-primary" href="#quiet-room">Get h-uman</a>
                  <a class="quiet-link" href="#quiet-room">See how it learns you</a>
                </div>
                <ul class="quiet-swatches" aria-label={`${mode} palette`}>
                  <li style="background: var(--hu-bg)">bg</li>
                  <li style="background: var(--hu-bg-inset)">inset</li>
                  <li style="background: var(--hu-bg-surface)">card</li>
                  <li style="background: var(--hu-accent); color: var(--hu-on-accent)">accent</li>
                  <li style="background: var(--hu-accent-brand)" aria-label="accent-brand, decorative only">brand</li>
                </ul>
              </div>
            ))}
          </div>
        </div>
      </section>
```

- Add to the page's existing `<style>` block (or, if `design.astro` has none, add `<style>` at the end of the file):

```css
  .quiet-lede { color: var(--hu-text-secondary); max-width: 60ch; margin-bottom: var(--hu-space-xl); }
  .quiet-pair { display: grid; gap: var(--hu-space-lg); grid-template-columns: repeat(auto-fit, minmax(min(100%, 22rem), 1fr)); }
  .quiet-panel { background: var(--hu-bg); color: var(--hu-text); border: 1px solid var(--hu-border); border-radius: var(--hu-radius-lg); padding: var(--hu-space-xl); }
  .quiet-eyebrow { font-size: var(--hu-text-2xs); letter-spacing: 0.18em; text-transform: uppercase; color: var(--hu-text-muted); margin-bottom: var(--hu-space-sm); }
  .quiet-hero { font-family: var(--hu-type-display-lg-family, var(--hu-font)); font-size: clamp(2.25rem, 1.5rem + 3vw, 3.75rem); font-weight: var(--hu-type-display-lg-weight); letter-spacing: var(--hu-type-display-lg-letter-spacing); line-height: 1; margin-bottom: var(--hu-space-md); }
  .quiet-hero em { color: var(--hu-accent-text); }
  .quiet-greeting { font-family: var(--hu-type-headline-md-family, var(--hu-font)); font-size: var(--hu-text-xl); margin-bottom: var(--hu-space-xs); }
  .quiet-body { color: var(--hu-text-secondary); margin-bottom: var(--hu-space-lg); }
  .quiet-actions { display: flex; flex-wrap: wrap; gap: var(--hu-space-md); align-items: center; margin-bottom: var(--hu-space-lg); }
  .quiet-primary { background: var(--hu-accent); color: var(--hu-on-accent); padding: var(--hu-space-sm) var(--hu-space-lg); border-radius: var(--hu-radius-full); text-decoration: none; }
  .quiet-primary:hover { background: var(--hu-accent-hover); }
  .quiet-link { color: var(--hu-link); }
  .quiet-primary:focus-visible, .quiet-link:focus-visible { outline: 2px solid var(--hu-focus-ring); outline-offset: 2px; }
  .quiet-swatches { display: flex; flex-wrap: wrap; gap: var(--hu-space-xs); list-style: none; padding: 0; font-size: var(--hu-text-2xs); }
  .quiet-swatches li { border: 1px solid var(--hu-border); border-radius: var(--hu-radius-sm); padding: var(--hu-space-sm) var(--hu-space-md); }
```

If `1px` borders are flagged by the website's lint (`astro check` does not lint tokens, but the repo-root `scripts/lint-raw-colors.sh` only checks colors), match whatever border-width token existing `design.astro` sections use. Read one existing card's styles first and copy its border declaration.

- [ ] **Step 5: Build and run everything for this task**

```bash
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui run check
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui run build
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/ui exec -- playwright test e2e/quiet-preview.spec.ts e2e/fonts-first-party.spec.ts e2e/quiet-cascade.spec.ts
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website run check
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website run build
npm --prefix /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website run check:fonts
grep -l 'data-brand=quiet\|data-brand="quiet"' /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website/dist/_astro/*.css
grep -c ':is(' $(grep -l 'data-brand' /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/website/dist/_astro/*.css)
```
Expected:
- `ui run check` passes, including `lint:tokens` with 0 violations.
- Playwright: 12 passed (1 + 2 + 9).
- `website check`, `build` and `check:fonts` exit 0.
- The two greps find the built stylesheet containing the quiet selectors, with at least one `:is(` surviving minification. This covers Review Focus #4 for the website. If `:is(` was lowered away, stop and report it: the specificity argument in spec II.2 would no longer hold.

- [ ] **Step 6: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add ui/src/views/design-system-view.ts ui/src/views/views.test.ts ui/e2e/quiet-preview.spec.ts ui/e2e/fonts-first-party.spec.ts website/src/pages/design.astro
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "feat(ui,website): Quiet Room specimens

/design gains a light/dark Quiet Room section rendered through the real
tokens; the dashboard's design-system view gains a session-wide preview
switch. The built bundle is verified to resolve quiet values (post-Vite),
and the website's built CSS keeps the :is() selectors.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Rules and documentation

**Files:**
- Modify: `CLAUDE.md` (the "Design System (all platforms)" section, ~line 185)
- Modify: `website/CLAUDE.md:33` (Font rule)
- Modify: `ui/CLAUDE.md` (Rules list; Token Lint section)
- Modify: `design-tokens/CLAUDE.md` (Token Files table; a new "Platform routing and checks" section)

**Interfaces:** documentation only.

- [ ] **Step 1: Apply the text changes**

`CLAUDE.md`: replace the line `- Typeface: **Avenir** (web: \`var(--hu-font)\`, never Google Fonts)` with:

```markdown
- Typeface: **Avenir** for UI and body on all platforms (web: `var(--hu-font)`); **Newsreader** (self-hosted, OFL) for display/headline roles on the web Quiet Room layer (`var(--hu-font-display)`). Never load fonts from Google or any third-party host.
```

`website/CLAUDE.md` line 33: replace with:

```markdown
- Fonts: Avenir via `var(--hu-font)` for UI/body; Newsreader via `var(--hu-font-display)` (Tailwind `font-display`) for display type in `data-brand="quiet"` scopes. Self-hosted only — `npm run check:fonts` fails on any Google font host in `dist/`.
```

`ui/CLAUDE.md`, in the Rules list after the SVG assets bullet, add:

```markdown
- **Fonts**: self-hosted only. Inter fallback lives in `public/fonts/inter/` (from `@fontsource-variable/inter` 5.3.0, OFL); Newsreader comes from `@fontsource-variable/newsreader`. `e2e/fonts-first-party.spec.ts` fails on any non-origin font request.
- **Quiet Room**: web-only layer toggled by `data-brand="quiet"` on `<html>` (design-system view has a preview switch). Components need no changes — they already read `--hu-*`.
```

`design-tokens/CLAUDE.md`: add `| \`quiet.tokens.json\` | Quiet Room web-only layer (paper & ink, OKLCH) — \`com.human.platform: "web"\` |` to the Token Files table, and append:

```markdown
## Platform routing and checks

- Every `*.tokens.json` declares `$extensions["com.human.platform"]`: `"all"` (CSS, Swift, Kotlin, C, docs JSON) or `"web"` (CSS only). Missing or unknown values fail the build.
- Native color emitters (`color-emit-lib.ts`) accept only `#RRGGBB` and `rgb()/rgba()`; anything else throws `UnsupportedColorError` instead of emitting black. Put `oklch()`/`display-p3` colors in a `"web"` file.
- `npm test` runs the `node:test` suites in `tests/`. `npm run check:contrast` measures every Quiet Room text/UI/fill pair in both modes (inherited values included) and exits 1 on any failure, 2 if there is nothing to measure. `npm run check` runs build + test + contrast.
```

- [ ] **Step 2: Run the docs gate**

Run: `bash /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec/scripts/doc-fleet.sh`
Expected: exit 0 (standards, terminology, frontmatter and markdown links all clean).

- [ ] **Step 3: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec add CLAUDE.md website/CLAUDE.md ui/CLAUDE.md design-tokens/CLAUDE.md
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec commit -m "docs: typeface, platform-routing and font-hosting rules for Quiet Room

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Acceptance run (spec II.7, A1–A10)

**Files:** none (verification only).

- [ ] **Step 1: Dispatch the verifier with the acceptance contract**

Use `/verify` (the `verifier` agent) with this contract, and require `RESULT_verifier=PASS`:

```
Worktree: /Users/sethford/Projects/h-uman/.claude/worktrees/quiet-room-spec (branch worktree-quiet-room-spec). Run everything; report observed output.
A1  git diff --stat origin/main -- apps/shared/HumanKit/Sources/HumanChatUI/DesignTokens.swift apps/android/app/src/main/java/ai/human/app/ui/DesignTokens.kt include/human/design_tokens.h  → empty
A2  bash design-tokens/check-drift.sh → exit 0
A3  add a temporary token {"$value":"color(display-p3 0.5 0.7 0.3)","$type":"color"} under light in design-tokens/semantic.tokens.json; npm --prefix design-tokens run build -- --outdir <tmp> → non-zero exit naming the value; revert; same build → exit 0
A4  npm --prefix design-tokens run check:contrast → exit 0 with "154 pairs measured, 0 failed"; npm --prefix design-tokens test → the "DISCRIMINATES" test passes
A5  grep ui/src/styles/_tokens.css for the four quiet selectors (present) and confirm no quiet value is declared inside a bare ":root {" block
A6  ui: playwright e2e/fonts-first-party.spec.ts → pass; website: npm run build && npm run check:fonts → 0 problems
A7  website: npm run check && npm run build → exit 0; Lighthouse on /design, median of 3 runs, vs the same on origin/main → no category drops > 3 points (report all six medians)
A8  ui: npm run check → exit 0 (typecheck, lint, format, lint:tokens 0, vitest)
A9  /verify-ui screenshots of website /design#quiet-room and dashboard /?demo#design-system with the switch on: light, dark, and prefers-contrast: more (in the last, high-contrast colors must win)
A10 CLAUDE.md, website/CLAUDE.md, ui/CLAUDE.md contain the Newsreader/self-hosting wording; ui/src/styles/theme.css contains no "gstatic"
Also: ui playwright e2e/quiet-cascade.spec.ts e2e/quiet-preview.spec.ts → all pass.
```

- [ ] **Step 2: One critic pass**

Dispatch the `critic` agent over `git diff origin/main...worktree-quiet-room-spec`, pointed at the spec. Allow at most two critic→fix→re-verify rounds (`~/.claude/rules/agent-team-os.md`). Any fix goes back through the owning task's tests.

- [ ] **Step 3: Hand off for merge**

Report the verifier's `RESULT_verifier` line, the check-contrast summary line, the Lighthouse medians and the critic outcome to the user. Merging to `main` is the user's call, done via PR in one short window (`.claude/rules/session-worktree-isolation.md`).
