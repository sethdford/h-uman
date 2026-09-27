import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { TEXT_ROLES } from "../../design-tokens/contrast-lib.ts";

// The Quiet Room layer is generated into its own file (the dashboard loads it
// with the design-system view); in the page it always follows _tokens.css.
const TOKENS_CSS = ["_tokens.css", "_quiet.css"]
  .map((f) => readFileSync(fileURLToPath(new URL(`../src/styles/${f}`, import.meta.url)), "utf-8"))
  .join("\n");
const HIGH_CONTRAST_CSS = readFileSync(
  fileURLToPath(new URL("../src/styles/high-contrast.css", import.meta.url)),
  "utf-8",
);
const QUIET = JSON.parse(
  readFileSync(fileURLToPath(new URL("../../design-tokens/quiet.tokens.json", import.meta.url)), "utf-8"),
).quiet;
const LIGHT_BG: string = QUIET.light.bg.$value;
const DARK_BG: string = QUIET.dark.bg.$value;
// getComputedStyle returns custom-property values verbatim — Chromium does
// not reserialize them. The mismatch is upstream of the browser: design-tokens'
// `postbuild` prettier pass reformats the committed _tokens.css, stripping
// insignificant trailing zeros (0.010 -> 0.01, 0.130 -> 0.13), while
// quiet.tokens.json's literal keeps the spec table's "0.010" / "0.130"
// spelling. Canonicalize decimal numbers the same way before comparing so
// the two sources agree regardless of formatting.
const norm = (s: string) =>
  s
    .trim()
    .replace(/\s+/g, " ")
    .replace(/\d+\.\d+/g, (n) => String(parseFloat(n)));

async function load(
  page: Page,
  htmlAttrs: string,
  body: string,
  media: { scheme: "light" | "dark"; p3?: boolean; contrastMore?: boolean; forcedColors?: boolean },
  extraCss = "",
) {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setEmulatedMedia", {
    features: [
      { name: "prefers-color-scheme", value: media.scheme },
      { name: "color-gamut", value: media.p3 ? "p3" : "srgb" },
      { name: "prefers-contrast", value: media.contrastMore ? "more" : "no-preference" },
      { name: "forced-colors", value: media.forcedColors ? "active" : "none" },
    ],
  });
  await page.setContent(
    `<!doctype html><html ${htmlAttrs}><head><style>${TOKENS_CSS}</style><style>${extraCss}</style></head><body>${body}</body></html>`,
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

  test("bare quiet container follows system dark", async ({ page }) => {
    await load(page, "", '<div id="q" data-brand="quiet"></div>', { scheme: "dark" });
    expect(await prop(page, "#q", "--hu-bg")).toBe(norm(DARK_BG));
  });

  test("html data-theme=dark reaches a bare quiet container", async ({ page }) => {
    await load(page, 'data-theme="dark"', '<div id="q" data-brand="quiet"></div>', { scheme: "light" });
    expect(await prop(page, "#q", "--hu-bg")).toBe(norm(DARK_BG));
  });

  // Correctness check for a supported configuration (data-theme on <html>);
  // it does not need to discriminate QUIET_LIGHT_ANCESTOR — see the comment
  // on that constant in quiet-lib.ts for why it structurally can't, here.
  test("html data-theme=light reaches a bare quiet container", async ({ page }) => {
    await load(page, 'data-theme="light"', '<div id="q" data-brand="quiet"></div>', { scheme: "dark" });
    expect(await prop(page, "#q", "--hu-bg")).toBe(norm(LIGHT_BG));
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

  // The base P3 block sets --hu-error (and friends) on :root. When the explicit
  // data-theme disagrees with the OS scheme it overrides the theme's inherited
  // value, so any semantic color the quiet layer does not own leaks a P3 value
  // that was never measured against paper. Every quiet override must win, and
  // no text role may resolve to a P3 value at all.
  for (const [mode, scheme] of [
    ["light", "dark"],
    ["dark", "light"],
  ] as const) {
    test(`P3, data-theme=${mode} on OS ${scheme}: every quiet ${mode} color wins, no text role is P3`, async ({
      page,
    }) => {
      await load(page, `data-brand="quiet" data-theme="${mode}"`, "", { scheme, p3: true });
      const colors = Object.entries(QUIET[mode] as Record<string, { $value: string; $type?: string }>).filter(
        ([, t]) => t.$type === "color",
      );
      expect(colors.length, "precondition: the quiet layer overrides colors in this mode").toBeGreaterThan(0);
      for (const [name, t] of colors) {
        expect(await prop(page, "html", `--hu-${name}`), `--hu-${name}`).toBe(norm(t.$value));
      }
      for (const role of TEXT_ROLES) {
        expect(await prop(page, "html", `--hu-${role}`), `--hu-${role}`).not.toMatch(/^color\(display-p3/);
      }
    });
  }

  test("prefers-contrast: more wins over quiet colors; quiet type still applies", async ({ page }) => {
    const hcBlock = TOKENS_CSS.match(/@media \(prefers-contrast: more\)\s*\{\s*:root\s*\{([^}]*)\}/);
    const hcText = hcBlock?.[1].match(/--hu-text:\s*([^;]+);/)?.[1];
    expect(hcText, "precondition: the high-contrast block defines --hu-text").toBeTruthy();
    await load(page, 'data-brand="quiet"', "", { scheme: "light", contrastMore: true });
    expect(await prop(page, "html", "--hu-text")).toBe(norm(hcText!));
    expect(await prop(page, "html", "--hu-font-display")).toContain("Newsreader Variable");
  });

  // high-contrast.css loads after _tokens.css, so any colour it sets under
  // prefers-contrast: more replaces the generated black palette. It used to set
  // text-faint #6b7280 (4.3:1 on black) and, under data-theme=light, dark-on-light
  // text and borders (~2:1) while the background stayed black.
  for (const [scheme, attrs] of [
    ["dark", ""],
    ["light", ""],
    ["dark", 'data-theme="light"'],
  ] as const) {
    test(`prefers-contrast: more, ${scheme} ${attrs || "no data-theme"}: generated palette survives high-contrast.css`, async ({
      page,
    }) => {
      await load(page, attrs, "", { scheme, contrastMore: true }, HIGH_CONTRAST_CSS);
      // Read the generated declarations through the CSSOM of the first <style>
      // (TOKENS_CSS), not a regex: any selector list or rule order the generator
      // emits is handled, and only rules that actually match <html> count.
      const generated = await page.evaluate(() => {
        const decls = new Map<string, string>();
        for (const rule of Array.from(document.styleSheets[0].cssRules)) {
          if (!(rule instanceof CSSMediaRule) || rule.conditionText !== "(prefers-contrast: more)") continue;
          for (const inner of Array.from(rule.cssRules)) {
            if (!(inner instanceof CSSStyleRule) || !document.documentElement.matches(inner.selectorText)) continue;
            for (let i = 0; i < inner.style.length; i++) {
              const name = inner.style[i];
              if (name.startsWith("--hu-")) decls.set(name, inner.style.getPropertyValue(name));
            }
          }
        }
        return [...decls];
      });
      const colors = generated.filter(([, v]) => !/px$/.test(v.trim()));
      expect(colors.length, "precondition: the high-contrast block defines colours").toBeGreaterThan(20);
      for (const [name, value] of colors) {
        expect(await prop(page, "html", name), name).toBe(norm(value));
      }
    });
  }

  // Forced colors (Windows High Contrast) does not imply prefers-contrast: more.
  // high-contrast.css's :root forced-colors overrides score (0,1,0); unguarded
  // quiet colors at (0,2,0) would beat them.
  test("forced-colors: active wins over quiet colors even without prefers-contrast: more", async ({ page }) => {
    await load(page, 'data-brand="quiet"', "", { scheme: "light", forcedColors: true }, HIGH_CONTRAST_CSS);
    expect(await page.evaluate(() => matchMedia("(forced-colors: active)").matches), "precondition").toBe(true);
    expect(await page.evaluate(() => matchMedia("(prefers-contrast: more)").matches), "precondition").toBe(false);
    expect(await prop(page, "html", "--hu-bg")).not.toBe(norm(LIGHT_BG));
    expect(await prop(page, "html", "--hu-bg")).toBe("Canvas");
  });
});
