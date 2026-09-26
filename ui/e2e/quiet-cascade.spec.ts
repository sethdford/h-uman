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

  test("prefers-contrast: more wins over quiet colors; quiet type still applies", async ({ page }) => {
    const hcBlock = TOKENS_CSS.match(/@media \(prefers-contrast: more\)\s*\{\s*:root\s*\{([^}]*)\}/);
    const hcText = hcBlock?.[1].match(/--hu-text:\s*([^;]+);/)?.[1];
    expect(hcText, "precondition: the high-contrast block defines --hu-text").toBeTruthy();
    await load(page, 'data-brand="quiet"', "", { scheme: "light", contrastMore: true });
    expect(await prop(page, "html", "--hu-text")).toBe(norm(hcText!));
    expect(await prop(page, "html", "--hu-font-display")).toContain("Newsreader Variable");
  });
});
