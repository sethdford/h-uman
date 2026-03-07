import { test, expect } from "@playwright/test";

test.describe("SeaClaw Control UI", () => {
  test("loads and shows the page title", async ({ page }) => {
    await page.goto("/");
    await expect(page).toHaveTitle(/SeaClaw Control/);
  });

  test("app shell renders with custom element", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("sc-app")).toBeAttached();
  });

  test("sidebar renders inside app shell", async ({ page }) => {
    await page.goto("/");
    const sidebar = page.locator("sc-app >> sc-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });
  });

  test("overview is the default view", async ({ page }) => {
    await page.goto("/");
    const overview = page.locator("sc-app >> sc-overview-view");
    await expect(overview).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads chat view", async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForTimeout(500);
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads models view", async ({ page }) => {
    await page.goto("/#models");
    await page.waitForTimeout(500);
    const modelsView = page.locator("sc-app >> sc-models-view");
    await expect(modelsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads tools view", async ({ page }) => {
    await page.goto("/#tools");
    await page.waitForTimeout(500);
    const toolsView = page.locator("sc-app >> sc-tools-view");
    await expect(toolsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads config view", async ({ page }) => {
    await page.goto("/#config");
    await page.waitForTimeout(500);
    const configView = page.locator("sc-app >> sc-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads logs view", async ({ page }) => {
    await page.goto("/#logs");
    await page.waitForTimeout(500);
    const logsView = page.locator("sc-app >> sc-logs-view");
    await expect(logsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads agents view", async ({ page }) => {
    await page.goto("/#agents");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-agents-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation #sessions redirects to chat", async ({ page }) => {
    await page.goto("/#sessions");
    await page.waitForTimeout(500);
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads channels view", async ({ page }) => {
    await page.goto("/#channels");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-channels-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads skills view", async ({ page }) => {
    await page.goto("/#skills");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-skills-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads automations view", async ({ page }) => {
    await page.goto("/#automations");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-automations-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads voice view", async ({ page }) => {
    await page.goto("/#voice");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-voice-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads nodes view", async ({ page }) => {
    await page.goto("/#nodes");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-nodes-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads usage view", async ({ page }) => {
    await page.goto("/#usage");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-usage-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads security view", async ({ page }) => {
    await page.goto("/#security");
    await page.waitForTimeout(500);
    const view = page.locator("sc-app >> sc-security-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("invalid hash falls back to overview", async ({ page }) => {
    await page.goto("/#nonexistent");
    await page.waitForTimeout(500);
    const overview = page.locator("sc-app >> sc-overview-view");
    await expect(overview).toBeAttached({ timeout: 5000 });
  });

  test("Ctrl+K opens command palette", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(300);
    await page.keyboard.press("Control+k");
    await page.waitForTimeout(300);
    const palette = page.locator("sc-app >> sc-command-palette");
    await expect(palette).toBeAttached({ timeout: 5000 });
  });

  test("Ctrl+B toggles sidebar collapsed state", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(300);
    const layout = page.locator("sc-app >> .layout");
    await expect(layout).toBeAttached({ timeout: 5000 });

    const hadCollapsed = await layout.evaluate((el) => el.classList.contains("collapsed"));
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(300);
    const hasCollapsed = await layout.evaluate((el) => el.classList.contains("collapsed"));
    expect(hasCollapsed).toBe(!hadCollapsed);
  });

  test("connection status shows disconnected initially", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(500);
    const sidebar = page.locator("sc-app >> sc-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });
  });

  test("floating mic button is present", async ({ page }) => {
    await page.goto("/");
    const mic = page.locator("sc-app >> sc-floating-mic");
    await expect(mic).toBeAttached({ timeout: 5000 });
  });

  test("navigating through all tabs sequentially works", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(300);

    const tabs = ["chat", "agents", "models", "tools", "channels", "skills", "overview"];

    for (const tab of tabs) {
      await page.evaluate((t) => (window.location.hash = t), tab);
      await page.waitForTimeout(400);
      const viewTag = tab === "overview" ? "sc-overview-view" : `sc-${tab}-view`;
      await expect(page.locator(`sc-app >> ${viewTag}`)).toBeAttached({
        timeout: 5000,
      });
    }
  });
});
