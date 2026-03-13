import { test, expect, type Page, type ConsoleMessage } from "@playwright/test";
import { ALL_VIEWS, VIEW_TAGS, deepText, shadowExists, waitForViewReady, POLL } from "./helpers.js";

/**
 * Regression test suite: catches unexpected errors across all views.
 *
 * For each view in demo mode, verifies:
 *  1. No JS console errors (beyond known allowlist)
 *  2. No error banners visible in the rendered view
 *  3. View renders populated content (not stuck in loading/error state)
 *  4. No uncaught exceptions during navigation
 */

const KNOWN_CONSOLE_ERRORS = ["Lit is in dev mode", "favicon.ico", "ERR_CONNECTION_REFUSED"];

function isKnownConsoleError(msg: string): boolean {
  return KNOWN_CONSOLE_ERRORS.some((known) => msg.includes(known));
}

test.describe("Regression: No Unexpected Errors (Demo Mode)", () => {
  for (const view of ALL_VIEWS) {
    const viewTag = VIEW_TAGS[view]!;

    test(`${view} — no console errors`, async ({ page }) => {
      const errors: string[] = [];
      page.on("console", (msg: ConsoleMessage) => {
        if (msg.type() === "error" && !isKnownConsoleError(msg.text())) {
          errors.push(msg.text());
        }
      });
      page.on("pageerror", (err) => {
        errors.push(`Uncaught: ${err.message}`);
      });

      await page.goto(`/?demo#${view}`);
      await waitForViewReady(page, viewTag);
      await page.waitForTimeout(2000);

      expect(errors, `Console errors on ${view} view: ${errors.join("; ")}`).toHaveLength(0);
    });

    test(`${view} — no error banner visible`, async ({ page }) => {
      await page.goto(`/?demo#${view}`);
      await waitForViewReady(page, viewTag);
      await page.waitForTimeout(2000);

      await expect(async () => {
        const hasErrorBanner = await page.evaluate(shadowExists(viewTag, ".error-banner"));
        expect(hasErrorBanner, `${view} view has an unexpected error banner`).toBe(false);
      }).toPass({ timeout: POLL });
    });

    test(`${view} — renders populated content`, async ({ page }) => {
      await page.goto(`/?demo#${view}`);
      await waitForViewReady(page, viewTag);
      await page.waitForTimeout(2000);

      await expect(async () => {
        const text: string = await page.evaluate(deepText(viewTag));
        expect(
          text.length,
          `${view} view has too little content (${text.length} chars) — may be stuck in loading/error state`,
        ).toBeGreaterThan(50);
      }).toPass({ timeout: POLL });
    });
  }
});

test.describe("Regression: Sequential Navigation (Demo Mode)", () => {
  test("navigating through all views produces no console errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("console", (msg: ConsoleMessage) => {
      if (msg.type() === "error" && !isKnownConsoleError(msg.text())) {
        errors.push(`[${msg.location().url}] ${msg.text()}`);
      }
    });
    page.on("pageerror", (err) => {
      errors.push(`Uncaught: ${err.message}`);
    });

    await page.goto("/?demo#overview");
    await waitForViewReady(page, "hu-overview-view");
    await page.waitForTimeout(1000);

    for (const view of ALL_VIEWS) {
      const viewTag = VIEW_TAGS[view]!;
      await page.evaluate((v) => (window.location.hash = v), view);
      await waitForViewReady(page, viewTag);
      await page.waitForTimeout(500);
    }

    expect(errors, `Console errors during navigation: ${errors.join("; ")}`).toHaveLength(0);
  });

  test("navigating through all views shows no error banners", async ({ page }) => {
    await page.goto("/?demo#overview");
    await waitForViewReady(page, "hu-overview-view");
    await page.waitForTimeout(1000);

    const viewsWithErrors: string[] = [];

    for (const view of ALL_VIEWS) {
      const viewTag = VIEW_TAGS[view]!;
      await page.evaluate((v) => (window.location.hash = v), view);
      await waitForViewReady(page, viewTag);
      await page.waitForTimeout(1500);

      const hasError = await page.evaluate(shadowExists(viewTag, ".error-banner"));
      if (hasError) viewsWithErrors.push(view);
    }

    expect(
      viewsWithErrors,
      `Views with unexpected error banners: ${viewsWithErrors.join(", ")}`,
    ).toHaveLength(0);
  });
});

test.describe("Regression: Rapid Navigation (Demo Mode)", () => {
  test("rapidly switching views does not crash or show errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => {
      errors.push(`Uncaught: ${err.message}`);
    });

    await page.goto("/?demo#overview");
    await waitForViewReady(page, "hu-overview-view");

    for (const view of ALL_VIEWS) {
      await page.evaluate((v) => (window.location.hash = v), view);
      await page.waitForTimeout(100);
    }
    await page.waitForTimeout(2000);

    expect(errors, `Uncaught errors during rapid nav: ${errors.join("; ")}`).toHaveLength(0);

    const lastView = ALL_VIEWS[ALL_VIEWS.length - 1]!;
    const lastTag = VIEW_TAGS[lastView]!;
    const attached = await page.evaluate((tag) => {
      const app = document.querySelector("hu-app");
      return !!app?.shadowRoot?.querySelector(tag);
    }, lastTag);
    expect(attached, `Last view (${lastView}) should be attached after rapid nav`).toBe(true);
  });
});
