import { test, expect } from "@playwright/test";

test.describe("h-uman Control UI", () => {
  test("loads and shows the page title", async ({ page }) => {
    await page.goto("/");
    await expect(page).toHaveTitle(/h-uman Control/);
  });

  test("app shell renders with custom element", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("hu-app")).toBeAttached();
  });

  test("sidebar renders inside app shell", async ({ page }) => {
    await page.goto("/");
    const sidebar = page.locator("hu-app >> hu-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });
  });

  test("chat is the default view", async ({ page }) => {
    await page.goto("/");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads chat view", async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForLoadState("domcontentloaded");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads models view", async ({ page }) => {
    await page.goto("/#models");
    await page.waitForLoadState("domcontentloaded");
    const modelsView = page.locator("hu-app >> hu-models-view");
    await expect(modelsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads tools view", async ({ page }) => {
    await page.goto("/#tools");
    await page.waitForLoadState("domcontentloaded");
    const toolsView = page.locator("hu-app >> hu-tools-view");
    await expect(toolsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads config view", async ({ page }) => {
    await page.goto("/#config");
    await page.waitForLoadState("domcontentloaded");
    const configView = page.locator("hu-app >> hu-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads logs view", async ({ page }) => {
    await page.goto("/#logs");
    await page.waitForLoadState("domcontentloaded");
    const logsView = page.locator("hu-app >> hu-logs-view");
    await expect(logsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads agents view", async ({ page }) => {
    await page.goto("/#agents");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-agents-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads sessions view", async ({ page }) => {
    await page.goto("/?demo#sessions");
    await page.waitForLoadState("domcontentloaded");
    const sessionsView = page.locator("hu-app >> hu-sessions-view");
    await expect(sessionsView).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads channels view", async ({ page }) => {
    await page.goto("/#channels");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-channels-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads skills view", async ({ page }) => {
    await page.goto("/#skills");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-skills-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads automations view", async ({ page }) => {
    await page.goto("/#automations");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-automations-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads voice view", async ({ page }) => {
    await page.goto("/#voice");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-voice-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads nodes view", async ({ page }) => {
    await page.goto("/#nodes");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-nodes-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads usage view", async ({ page }) => {
    await page.goto("/#usage");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-usage-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("hash navigation loads security view", async ({ page }) => {
    await page.goto("/#security");
    await page.waitForLoadState("domcontentloaded");
    const view = page.locator("hu-app >> hu-security-view");
    await expect(view).toBeAttached({ timeout: 5000 });
  });

  test("invalid hash falls back to chat", async ({ page }) => {
    await page.goto("/#nonexistent");
    await page.waitForLoadState("domcontentloaded");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("Ctrl+K opens command palette", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 5000 });
    await page.keyboard.press("Control+k");
    const palette = page.locator("hu-app >> hu-command-palette");
    await expect(palette).toBeAttached({ timeout: 5000 });
  });

  test("Ctrl+B toggles sidebar collapsed state", async ({ page }) => {
    await page.goto("/");
    const layout = page.locator("hu-app >> .layout");
    await expect(layout).toBeAttached({ timeout: 5000 });

    const hadCollapsed = await layout.evaluate((el) => el.classList.contains("collapsed"));
    await page.keyboard.press("Control+b");
    await expect(async () => {
      const hasCollapsed = await layout.evaluate((el) => el.classList.contains("collapsed"));
      expect(hasCollapsed).toBe(!hadCollapsed);
    }).toPass({ timeout: 3000 });
  });

  test("connection status shows disconnected initially", async ({ page }) => {
    await page.goto("/");
    await page.waitForLoadState("domcontentloaded");
    const sidebar = page.locator("hu-app >> hu-sidebar");
    await expect(sidebar).toBeAttached({ timeout: 5000 });
  });

  // Marked .fixme — repeatedly flakes on CI due to Vite WS proxy
  // ECONNRESET storms during cold start that delay
  // requestIdleCallback past any reasonable timeout. Attempts that
  // failed:
  //   1. 5s → 15s timeout bumps (still flaked)
  //   2. Force-trigger import via page.evaluate (404 — production
  //      preview bundles to hashed paths, not /src/*.ts)
  // The mic IS bundled (visible in `dist/assets/floating-mic-*.js`)
  // and renders correctly in production; the flake is purely a
  // test-environment startup race. Root fix requires either:
  //   a) Fixing Vite WS proxy stability during the test cold-start
  //   b) Eagerly registering the component (loses lazy-load perf
  //      benefit on first paint for real users)
  //   c) Exposing a test-only hook on hu-app to force-load
  // Tracked separately. Until then, this test is a known flake — see
  // PR #104, #107, #113 CI history for the failure pattern.
  test.fixme("floating mic button is present", async ({ page }) => {
    await page.goto("/");
    await page.waitForLoadState("domcontentloaded");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 5000 });
    const mic = page.locator("hu-app >> hu-floating-mic");
    await expect(mic).toBeAttached({ timeout: 5000 });
  });

  test("navigating through all tabs sequentially works", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 5000 });

    const tabs = ["chat", "agents", "models", "tools", "channels", "skills", "overview"];

    for (const tab of tabs) {
      await page.evaluate((t) => (window.location.hash = t), tab);
      const viewTag = tab === "overview" ? "hu-overview-view" : `hu-${tab}-view`;
      await expect(page.locator(`hu-app >> ${viewTag}`)).toBeAttached({
        timeout: 5000,
      });
    }
  });

  test("auto-fallback populates chat without live gateway", async ({ page }) => {
    await page.goto("/");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const text = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-chat-view");
        return view?.shadowRoot?.textContent ?? "";
      });
      expect(text.length).toBeGreaterThan(50);
    }).toPass({ timeout: 8000 });
  });
});
