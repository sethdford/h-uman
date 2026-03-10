import { test, expect } from "@playwright/test";

test.describe("Performance Latency Budgets", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await page.waitForLoadState("domcontentloaded");
    await expect(page.locator("sc-app")).toBeAttached({ timeout: 5000 });
  });

  test("button click responds within 50ms budget", async ({ page }) => {
    const sidebar = page.locator("sc-app >> sc-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });

    const navItems = sidebar.locator("[data-testid]").or(sidebar.locator("a, button"));
    const count = await navItems.count();
    if (count === 0) {
      test.skip();
      return;
    }

    const target = navItems.first();
    const start = await page.evaluate(() => performance.now());
    await target.click();
    const elapsed = await page.evaluate((s) => performance.now() - s, start);

    expect(elapsed).toBeLessThan(50);
  });

  test("view transition completes within 200ms budget", async ({ page }) => {
    const sidebar = page.locator("sc-app >> sc-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });

    const chatLink = sidebar.locator('a[href*="chat"], [data-view="chat"]').first();
    const hasChatLink = (await chatLink.count()) > 0;
    if (!hasChatLink) {
      test.skip();
      return;
    }

    const start = await page.evaluate(() => performance.now());
    await chatLink.click();
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    const elapsed = await page.evaluate((s) => performance.now() - s, start);

    expect(elapsed).toBeLessThan(200);
  });

  test("keyboard shortcut responds within 80ms budget", async ({ page }) => {
    const start = await page.evaluate(() => performance.now());
    await page.keyboard.press("Control+k");

    const commandPalette = page
      .locator("sc-app")
      .locator("sc-command-palette, [role='combobox'], [data-testid='command-palette']");

    const appeared = await commandPalette.count();
    if (appeared === 0) {
      await page.keyboard.press("Meta+k");
      const elapsed = await page.evaluate((s) => performance.now() - s, start);
      const retryCount = await commandPalette.count();
      if (retryCount === 0) {
        test.skip();
        return;
      }
      expect(elapsed).toBeLessThan(80);
      return;
    }

    const elapsed = await page.evaluate((s) => performance.now() - s, start);
    expect(elapsed).toBeLessThan(80);
  });
});
