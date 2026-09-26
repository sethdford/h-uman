import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";

/**
 * Wide-gamut (P3) theme cascade.
 *
 * `_tokens.css` ends with a `(color-gamut: p3)` block of vivid overrides. It
 * must only touch the keys that the ACTIVE theme's P3 map declares; every
 * other key keeps the value the sRGB theme gives it. Before the fix, the dark
 * P3 map was emitted unconditionally, so on a P3 display the light theme got
 * the dark link / focus ring (2.14:1 / 2.55:1 on the light background), an
 * explicit data-theme toggle was ignored, and the high-contrast theme was
 * overridden.
 *
 * Each case renders the real stylesheet twice under CDP media emulation —
 * once as P3, once as sRGB — and requires P3 == sRGB for every key the active
 * theme has no P3 value for.
 */

const TOKENS_CSS = readFileSync(new URL("../src/styles/_tokens.css", import.meta.url), "utf8");
const P3_SOURCE = JSON.parse(
  readFileSync(new URL("../../design-tokens/semantic.tokens.json", import.meta.url), "utf8"),
).$extensions["human.p3Colors"] as Record<string, string>;

function p3Map(theme: "dark" | "light"): Record<string, string> {
  return Object.fromEntries(
    Object.entries(P3_SOURCE)
      .filter(([k]) => k.startsWith(`${theme}.`))
      .map(([k, v]) => [k.slice(theme.length + 1), v]),
  );
}

const DARK_P3 = p3Map("dark");
const LIGHT_P3 = p3Map("light");
const P3_KEYS = [...new Set([...Object.keys(DARK_P3), ...Object.keys(LIGHT_P3)])].sort();

interface Env {
  scheme: "light" | "dark";
  contrast?: "more" | "no-preference";
  dataTheme?: "light" | "dark";
}

interface Rendered {
  /** computed color of var(--hu-<key>) for every P3 key, plus "bg" */
  tokens: Record<string, string>;
  /** computed color of each literal P3 source value, keyed by that value */
  literals: Record<string, string>;
}

async function render(page: Page, env: Env, gamut: "p3" | "srgb"): Promise<Rendered> {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", {
    features: [
      { name: "color-gamut", value: gamut },
      { name: "prefers-color-scheme", value: env.scheme },
      { name: "prefers-contrast", value: env.contrast ?? "no-preference" },
    ],
  });
  const attr = env.dataTheme ? ` data-theme="${env.dataTheme}"` : "";
  await page.setContent(
    `<!doctype html><html${attr}><head><style>${TOKENS_CSS}</style></head><body></body></html>`,
  );

  // Precondition: the emulation actually took, so a pass is not vacuous.
  const media = await page.evaluate(() => ({
    p3: matchMedia("(color-gamut: p3)").matches,
    light: matchMedia("(prefers-color-scheme: light)").matches,
    more: matchMedia("(prefers-contrast: more)").matches,
  }));
  expect(media).toEqual({
    p3: gamut === "p3",
    light: env.scheme === "light",
    more: env.contrast === "more",
  });

  return page.evaluate(
    ({ keys, literals }) => {
      const probe = document.createElement("div");
      document.body.append(probe);
      const resolve = (expr: string) => {
        probe.style.color = "";
        probe.style.color = expr;
        return getComputedStyle(probe).color;
      };
      const tokens: Record<string, string> = {};
      for (const k of [...keys, "bg"]) tokens[k] = resolve(`var(--hu-${k})`);
      const lit: Record<string, string> = {};
      for (const v of literals) lit[v] = resolve(v);
      return { tokens, literals: lit };
    },
    { keys: P3_KEYS, literals: Object.values(P3_SOURCE) },
  );
}

/** What every P3 key should resolve to when `activeP3` is the live P3 map. */
function expected(
  srgb: Rendered,
  p3: Rendered,
  activeP3: Record<string, string>,
): Record<string, string> {
  return Object.fromEntries(
    P3_KEYS.map((k) => [k, k in activeP3 ? p3.literals[activeP3[k]] : srgb.tokens[k]]),
  );
}

function tokensOnly(r: Rendered): Record<string, string> {
  return Object.fromEntries(P3_KEYS.map((k) => [k, r.tokens[k]]));
}

// WCAG 2.x contrast. display-p3 uses the sRGB transfer curve with P3 primaries.
function luminance(css: string): number {
  const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const num = String.raw`([\d.e+-]+)`;
  const p3 = css.match(new RegExp(String.raw`^color\(\s*display-p3\s+${num}\s+${num}\s+${num}`));
  if (p3) {
    const [r, g, b] = p3.slice(1, 4).map((x) => lin(Number(x)));
    return 0.2289746 * r + 0.6917385 * g + 0.0792869 * b;
  }
  // Legacy "rgb(1, 2, 3)" and modern "rgb(1 2 3)" / "rgb(1 2 3 / a)" forms.
  const rgb = css.match(new RegExp(String.raw`^rgba?\(\s*${num}[\s,]+${num}[\s,]+${num}`));
  if (!rgb) throw new Error(`unparseable color: ${css}`);
  const [r, g, b] = rgb.slice(1, 4).map((x) => lin(Number(x) / 255));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function contrast(a: string, b: string): number {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

const CASES: { name: string; env: Env; active: Record<string, string> }[] = [
  { name: "light scheme", env: { scheme: "light" }, active: LIGHT_P3 },
  { name: "dark scheme", env: { scheme: "dark" }, active: DARK_P3 },
  {
    name: 'data-theme="light" on a dark-scheme system',
    env: { scheme: "dark", dataTheme: "light" },
    active: LIGHT_P3,
  },
  {
    name: 'data-theme="dark" on a light-scheme system',
    env: { scheme: "light", dataTheme: "dark" },
    active: DARK_P3,
  },
  {
    name: "high contrast, light scheme",
    env: { scheme: "light", contrast: "more" },
    active: {},
  },
  {
    name: "high contrast, dark scheme",
    env: { scheme: "dark", contrast: "more" },
    active: {},
  },
];

test.describe("P3 overrides follow the active theme", () => {
  for (const { name, env, active } of CASES) {
    test(name, async ({ page }) => {
      const srgb = await render(page, env, "srgb");
      const p3 = await render(page, env, "p3");
      expect(tokensOnly(p3)).toEqual(expected(srgb, p3, active));
    });
  }

  test("light theme on P3 keeps link, focus ring and accent text legible", async ({ page }) => {
    const { tokens, literals } = await render(page, { scheme: "light" }, "p3");
    // Precondition: the P3 block is live (light accent is a P3 color).
    expect(tokens.accent).toMatch(/^color\(display-p3/);
    expect(tokens.link).not.toBe(literals[DARK_P3.link]);
    expect(tokens["focus-ring"]).not.toBe(literals[DARK_P3["focus-ring"]]);
    // WCAG 1.4.11 non-text (3:1) and 1.4.3 text (4.5:1).
    expect(contrast(tokens["focus-ring"], tokens.bg)).toBeGreaterThanOrEqual(3);
    expect(contrast(tokens["accent-text"], tokens.bg)).toBeGreaterThanOrEqual(4.5);
  });
});
