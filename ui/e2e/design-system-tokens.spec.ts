import { test, expect } from "@playwright/test";
import {
  VIEW_TAGS,
  ALL_VIEWS,
  WAIT,
  POLL,
  shadowComputedStyle,
  isKnownPageError,
} from "./helpers.js";

/**
 * Design system token verification tests.
 *
 * Ensures M3 tonal surfaces, tinted state layers, glass effects, and
 * dynamic color tokens are correctly applied in production. Prevents
 * regression of design token migration.
 */

function getComputedToken(token: string) {
  return `(() => {
    return getComputedStyle(document.documentElement).getPropertyValue("${token}").trim();
  })()`;
}

function getSidebarStyle(prop: string) {
  return `(() => {
    const app = document.querySelector("hu-app");
    const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
    if (!sidebar) return "";
    return getComputedStyle(sidebar).getPropertyValue("${prop}").trim();
  })()`;
}

test.describe("Design Tokens — Core Tokens Present", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForLoadState("domcontentloaded");
  });

  test("tonal surface tokens are defined", async ({ page }) => {
    const container = await page.evaluate(getComputedToken("--hu-surface-container"));
    const high = await page.evaluate(getComputedToken("--hu-surface-container-high"));
    const highest = await page.evaluate(getComputedToken("--hu-surface-container-highest"));

    expect(container).toBeTruthy();
    expect(high).toBeTruthy();
    expect(highest).toBeTruthy();
  });

  test("tinted overlay tokens are defined", async ({ page }) => {
    const hover = await page.evaluate(getComputedToken("--hu-hover-overlay"));
    const pressed = await page.evaluate(getComputedToken("--hu-pressed-overlay"));
    const focus = await page.evaluate(getComputedToken("--hu-focus-overlay"));

    expect(hover).toBeTruthy();
    expect(pressed).toBeTruthy();
    expect(focus).toBeTruthy();
  });

  test("glass tokens are defined", async ({ page }) => {
    const blur = await page.evaluate(getComputedToken("--hu-glass-standard-blur"));
    const saturate = await page.evaluate(getComputedToken("--hu-glass-standard-saturate"));
    const bgOpacity = await page.evaluate(getComputedToken("--hu-glass-standard-bg-opacity"));

    expect(blur).toBeTruthy();
    expect(saturate).toBeTruthy();
    expect(bgOpacity).toBeTruthy();
  });

  test("dynamic color tokens are defined", async ({ page }) => {
    const primary500 = await page.evaluate(getComputedToken("--hu-dynamic-primary-500"));
    expect(primary500).toBeTruthy();
  });

  test("sidebar width tokens are defined", async ({ page }) => {
    const width = await page.evaluate(getComputedToken("--hu-sidebar-width"));
    const collapsed = await page.evaluate(getComputedToken("--hu-sidebar-collapsed"));

    expect(width).toBeTruthy();
    expect(collapsed).toBeTruthy();
  });
});

test.describe("Design Tokens — Sidebar Glass Effect", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForLoadState("domcontentloaded");
    await page.locator("hu-app >> hu-sidebar").waitFor({ timeout: 5000 });
  });

  test("sidebar has backdrop-filter applied", async ({ page }) => {
    const bdFilter = await page.evaluate(getSidebarStyle("backdrop-filter"));
    const wkFilter = await page.evaluate(getSidebarStyle("-webkit-backdrop-filter"));
    const hasFilter = bdFilter.includes("blur") || wkFilter.includes("blur");
    expect(hasFilter).toBe(true);
  });

  test("sidebar border uses translucent color-mix", async ({ page }) => {
    const borderRight = await page.evaluate(getSidebarStyle("border-right"));
    expect(borderRight).toBeTruthy();
  });
});

test.describe("Design Tokens — Theme Parity", () => {
  const criticalTokens = [
    "--hu-surface-container",
    "--hu-surface-container-high",
    "--hu-surface-container-highest",
    "--hu-hover-overlay",
    "--hu-pressed-overlay",
    "--hu-bg",
    "--hu-text",
  ];

  // Read the token's computed value AFTER setting data-theme — combine
  // both into a single page.evaluate with an rAF flush so the browser
  // has actually applied the style invalidation before getComputedStyle
  // reads. Without the rAF, the read can race the invalidation and
  // return the previous theme's value (intermittent flake observed on
  // PR #113 round 7 for --hu-surface-container-high specifically).
  const readTokenForTheme = (theme: string, token: string) =>
    `(async () => {
      document.documentElement.setAttribute("data-theme", "${theme}");
      // Force two rAFs to flush style + layout — first paints the
      // attribute change, second guarantees getComputedStyle reads it.
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      return getComputedStyle(document.documentElement).getPropertyValue("${token}").trim();
    })()`;

  for (const token of criticalTokens) {
    test(`${token} differs between light and dark themes`, async ({ page }) => {
      await page.goto("/?demo");
      await page.waitForLoadState("domcontentloaded");

      const lightVal = await page.evaluate(readTokenForTheme("light", token));
      const darkVal = await page.evaluate(readTokenForTheme("dark", token));

      expect(lightVal).toBeTruthy();
      expect(darkVal).toBeTruthy();
      expect(lightVal).not.toEqual(darkVal);
    });
  }
});

test.describe("Design Tokens — No Console Errors", () => {
  for (const view of ALL_VIEWS) {
    test(`${view} view loads without JS errors`, async ({ page }) => {
      const errors: string[] = [];
      page.on("pageerror", (err) => {
        if (!isKnownPageError(err.message)) errors.push(err.message);
      });

      await page.goto(`/?demo#${view}`);
      await page.waitForTimeout(WAIT);

      expect(errors).toEqual([]);
    });
  }
});
