import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";

/**
 * prefers-contrast: more must stay legible in BOTH color schemes.
 *
 * The high-contrast palette used to be one dark-oriented set (#fff text, #000
 * bg) emitted on bare :root with no color-scheme condition, and
 * high-contrast.css added white-alpha surfaces on bare :root as well. Under the
 * light scheme that left white text on the light theme's #f8f8f6 cards.
 *
 * Each case renders the stylesheets the dashboard loads, in theme.css import
 * order, under CDP media emulation and measures WCAG 2.x contrast of every
 * foreground role on every background role the UI paints them on.
 */

const read = (p: string) => readFileSync(new URL(p, import.meta.url), "utf8");
const CSS = read("../src/styles/_tokens.css") + "\n" + read("../src/styles/high-contrast.css");

const BACKGROUNDS = [
  "bg",
  "bg-inset",
  "bg-surface",
  "bg-elevated",
  "bg-overlay",
  "surface-container",
  "surface-container-high",
  "surface-container-highest",
];

/** Minimum ratio per foreground role. 7:1 is the AAA body-text bar the
 *  accessibility docs promise for high-contrast mode; 4.5:1 (AA text) for the
 *  quieter and coloured text roles; 3:1 (1.4.11 non-text) for UI boundaries. */
const FOREGROUNDS: Record<string, number> = {
  text: 7,
  "text-secondary": 7,
  "text-muted": 7,
  "text-tertiary": 7,
  "text-faint": 4.5,
  "accent-text": 4.5,
  "accent-secondary-text": 4.5,
  "accent-tertiary-text": 4.5,
  link: 4.5,
  "link-hover": 4.5,
  "link-active": 4.5,
  "link-visited": 4.5,
  success: 4.5,
  warning: 4.5,
  error: 4.5,
  info: 4.5,
  border: 3,
  accent: 3,
  "focus-ring": 3,
};

interface Env {
  scheme: "light" | "dark";
  dataTheme?: "light" | "dark";
  /** which palette polarity the page must end up with */
  expect: "light" | "dark";
}

const CASES: { name: string; env: Env }[] = [
  { name: "light scheme", env: { scheme: "light", expect: "light" } },
  { name: "dark scheme", env: { scheme: "dark", expect: "dark" } },
  {
    name: 'data-theme="light" on a dark-scheme system',
    env: { scheme: "dark", dataTheme: "light", expect: "light" },
  },
  {
    name: 'data-theme="dark" on a light-scheme system',
    env: { scheme: "light", dataTheme: "dark", expect: "dark" },
  },
];

async function render(page: Page, env: Env): Promise<Record<string, string>> {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", {
    features: [
      { name: "prefers-contrast", value: "more" },
      { name: "prefers-color-scheme", value: env.scheme },
    ],
  });
  const attr = env.dataTheme ? ` data-theme="${env.dataTheme}"` : "";
  await page.setContent(
    `<!doctype html><html${attr}><head><style>${CSS}</style></head><body></body></html>`,
  );

  // Precondition: the emulation took, so a pass is not vacuous.
  const media = await page.evaluate(() => ({
    more: matchMedia("(prefers-contrast: more)").matches,
    light: matchMedia("(prefers-color-scheme: light)").matches,
  }));
  expect(media).toEqual({ more: true, light: env.scheme === "light" });

  return page.evaluate(
    (keys) => {
      const probe = document.createElement("div");
      document.body.append(probe);
      const out: Record<string, string> = {};
      for (const k of keys) {
        probe.style.color = "";
        probe.style.color = `var(--hu-${k})`;
        out[k] = getComputedStyle(probe).color;
      }
      return out;
    },
    [...BACKGROUNDS, ...Object.keys(FOREGROUNDS)],
  );
}

// WCAG 2.x relative luminance of an opaque computed sRGB color. A translucent
// value has no contrast of its own (it depends on what is behind it), so it is
// refused rather than measured as if it were opaque.
function luminance(role: string, css: string): number {
  const m = css.match(/^rgba?\((\d+), (\d+), (\d+)(?:, ([\d.]+))?\)$/);
  if (!m) throw new Error(`--hu-${role}: unparseable computed color ${css}`);
  if (m[4] !== undefined && Number(m[4]) < 1) {
    throw new Error(`--hu-${role}: translucent (${css}); contrast is not measurable`);
  }
  const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const [r, g, b] = m.slice(1, 4).map((x) => lin(Number(x) / 255));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function contrast(fgRole: string, fg: string, bgRole: string, bg: string): number {
  const [hi, lo] = [luminance(fgRole, fg), luminance(bgRole, bg)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

test.describe("prefers-contrast: more is legible in both schemes", () => {
  for (const { name, env } of CASES) {
    test(name, async ({ page }) => {
      const c = await render(page, env);

      // Polarity: a light page has a lighter background than its text.
      const bgLighter = luminance("bg", c.bg) > luminance("text", c.text);
      expect(bgLighter, `bg ${c.bg} vs text ${c.text}`).toBe(env.expect === "light");

      // Every pair, reported together so one run shows the whole picture.
      const failures: string[] = [];
      for (const [fg, min] of Object.entries(FOREGROUNDS)) {
        for (const bg of BACKGROUNDS) {
          const ratio = contrast(fg, c[fg], bg, c[bg]);
          if (ratio < min) {
            failures.push(`${fg} ${c[fg]} on ${bg} ${c[bg]}: ${ratio.toFixed(2)} < ${min}`);
          }
        }
      }
      expect(failures).toEqual([]);

      // The headline pair from the bug report, asserted on its own.
      expect(
        contrast("text", c.text, "surface-container", c["surface-container"]),
      ).toBeGreaterThanOrEqual(7);
    });
  }
});
