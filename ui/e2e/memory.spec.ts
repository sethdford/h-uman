import { test, expect } from "@playwright/test";
import {
  shadowExists,
  shadowCount,
  deepText,
  waitForViewReady,
  waitForShadowSelector,
  WAIT,
  POLL,
} from "./helpers.js";

const VIEW = "hu-memory-view";

test.describe("Memory View", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#memory");
    await waitForViewReady(page, VIEW);
    await page.waitForTimeout(WAIT);
  });

  test("renders page hero with Memory heading", async ({ page }) => {
    await waitForShadowSelector(page, VIEW, "hu-stat-card");
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-page-hero"))).toBe(true);
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toMatch(/Memor/i);
    }).toPass({ timeout: POLL });
  });

  test("shows stat cards on load", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-stats-row"))).toBe(true);
      const count = await page.evaluate(shadowCount(VIEW, "hu-stat-card"));
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("displays memory entries as cards", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount(VIEW, ".memory-grid hu-card"));
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: POLL });
  });

  test("cards show key, content, and category badge", async ({ page }) => {
    await expect(async () => {
      const hasKey = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          return !!view?.shadowRoot?.querySelector(".memory-card .key");
        })()`,
      );
      const hasContent = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          return !!view?.shadowRoot?.querySelector(".memory-card .content");
        })()`,
      );
      const hasBadge = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          return !!view?.shadowRoot?.querySelector(".memory-card hu-badge");
        })()`,
      );
      expect(hasKey).toBe(true);
      expect(hasContent).toBe(true);
      expect(hasBadge).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("search input is present", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-input"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("category filter is present with segments", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-segmented-control"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("consolidate button is present", async ({ page }) => {
    await expect(async () => {
      const hasConsolidate = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const buttons = view?.shadowRoot?.querySelectorAll("hu-button");
          for (const btn of buttons ?? []) {
            if (btn.textContent?.trim().includes("Consolidate")) return true;
          }
          return false;
        })()`,
      );
      expect(hasConsolidate).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("clicking consolidate triggers reload", async ({ page }) => {
    const clicked = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector("${VIEW}");
      const buttons = view?.shadowRoot?.querySelectorAll("hu-button");
      for (const btn of buttons ?? []) {
        if (btn.textContent?.trim().includes("Consolidate")) {
          const inner = btn.shadowRoot?.querySelector("button");
          if (inner) { inner.click(); return true; }
        }
      }
      return false;
    })()`);
    expect(clicked).toBe(true);

    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-stats-row"))).toBe(true);
      const count = await page.evaluate(shadowCount(VIEW, "hu-stat-card"));
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });
});
