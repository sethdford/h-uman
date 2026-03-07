import { test, expect } from "@playwright/test";

test.describe("Secondary Views", () => {
  test("config view renders", async ({ page }) => {
    await page.goto("/#config");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const configView = app.locator("sc-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("models view renders", async ({ page }) => {
    await page.goto("/#models");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const modelsView = app.locator("sc-models-view");
    await expect(modelsView).toBeAttached({ timeout: 5000 });
  });

  test("tools view renders", async ({ page }) => {
    await page.goto("/#tools");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const toolsView = app.locator("sc-tools-view");
    await expect(toolsView).toBeAttached({ timeout: 5000 });
  });

  test("sessions view renders", async ({ page }) => {
    await page.goto("/#sessions");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const sessionsView = app.locator("sc-sessions-view");
    await expect(sessionsView).toBeAttached({ timeout: 5000 });
  });

  test("nodes view renders", async ({ page }) => {
    await page.goto("/#nodes");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const nodesView = app.locator("sc-nodes-view");
    await expect(nodesView).toBeAttached({ timeout: 5000 });
  });

  test("agents view renders", async ({ page }) => {
    await page.goto("/#agents");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const agentsView = app.locator("sc-agents-view");
    await expect(agentsView).toBeAttached({ timeout: 5000 });
  });

  test("automations view renders", async ({ page }) => {
    await page.goto("/#automations");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const automationsView = app.locator("sc-automations-view");
    await expect(automationsView).toBeAttached({ timeout: 5000 });
  });

  test("skills view renders", async ({ page }) => {
    await page.goto("/#skills");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const skillsView = app.locator("sc-skills-view");
    await expect(skillsView).toBeAttached({ timeout: 5000 });
  });

  test("voice view renders", async ({ page }) => {
    await page.goto("/#voice");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const voiceView = app.locator("sc-voice-view");
    await expect(voiceView).toBeAttached({ timeout: 5000 });
  });

  test("usage view renders", async ({ page }) => {
    await page.goto("/#usage");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const usageView = app.locator("sc-usage-view");
    await expect(usageView).toBeAttached({ timeout: 5000 });
  });

  test("security view renders", async ({ page }) => {
    await page.goto("/#security");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const securityView = app.locator("sc-security-view");
    await expect(securityView).toBeAttached({ timeout: 5000 });
  });

  test("logs view renders", async ({ page }) => {
    await page.goto("/#logs");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const logsView = app.locator("sc-logs-view");
    await expect(logsView).toBeAttached({ timeout: 5000 });
  });

  test("overview view renders", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const overviewView = app.locator("sc-overview-view");
    await expect(overviewView).toBeAttached({ timeout: 5000 });
  });

  test("navigation between views works", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(500);

    // Navigate to chat
    await page.goto("/#chat");
    await page.waitForTimeout(300);
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });

    // Navigate to config
    await page.goto("/#config");
    await page.waitForTimeout(300);
    const configView = page.locator("sc-app >> sc-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("unknown hash falls back gracefully", async ({ page }) => {
    await page.goto("/#nonexistent");
    await page.waitForTimeout(500);
    // Should not crash, should show some view
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
  });
});
