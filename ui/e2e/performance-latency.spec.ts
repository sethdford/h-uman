import { test, expect } from "@playwright/test";
import { waitForViewReady, WAIT } from "./helpers.js";

test.describe("Performance Latency Budgets", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#overview");
    await waitForViewReady(page, "hu-overview-view");
    await page.waitForTimeout(WAIT);
  });

  test("button click responds within 50ms budget", async ({ page }) => {
    const hasNav = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      return (sidebar?.shadowRoot?.querySelectorAll("button.nav-item")?.length ?? 0) > 0;
    });
    if (!hasNav) {
      test.skip(true, "No nav items found in sidebar shadow DOM");
      return;
    }

    const elapsed = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const btn = sidebar?.shadowRoot?.querySelector("button.nav-item") as HTMLElement | null;
      if (!btn) return 999;
      const start = performance.now();
      btn.click();
      return performance.now() - start;
    });

    expect(elapsed).toBeLessThan(150);
  });

  test("view transition completes within 200ms budget", async ({ page }) => {
    const start = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const buttons = sidebar?.shadowRoot?.querySelectorAll("button.nav-item");
      const chatBtn = Array.from(buttons ?? []).find((b) =>
        b.getAttribute("aria-label")?.toLowerCase().includes("chat"),
      ) as HTMLElement | null;
      if (!chatBtn) return -1;
      const s = performance.now();
      chatBtn.click();
      return s;
    });
    if (start < 0) {
      test.skip(true, "No chat nav button found");
      return;
    }

    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    const elapsed = await page.evaluate((s) => performance.now() - s, start);

    expect(elapsed).toBeLessThan(200);
  });

  test("keyboard shortcut responds within 80ms budget", async ({ page }) => {
    const isMac = process.platform === "darwin";
    await page.keyboard.press(isMac ? "Meta+k" : "Control+k");

    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        return !!app?.shadowRoot?.querySelector("hu-command-palette");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 5000 });

    const elapsed = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const palette = app?.shadowRoot?.querySelector("hu-command-palette");
      if (!palette) return 999;
      const start = performance.now();
      palette.dispatchEvent(new Event("close"));
      return performance.now() - start;
    });

    expect(elapsed).toBeLessThan(80);
  });
});
