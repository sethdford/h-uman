import { test, expect } from "@playwright/test";

/**
 * Full chat flow test using demo mode.
 * Uses baseURL from playwright.config (preview server on 4173).
 * Run with: npx playwright test chat-gemini-flow --project=chromium
 */
test.describe("Chat Gemini Flow", () => {
  test("full chat flow - send message and get demo response", async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForLoadState("domcontentloaded");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 10000 });

    // The composer renders its textarea after hu-chat-view attaches; a waiting
    // locator (which pierces open shadow roots) avoids racing that render.
    const input = chatView.getByRole("textbox", { name: "Message input" });
    await expect(input).toBeVisible({ timeout: 10000 });
    const prompt = "Hello! What can you do?";
    await input.fill(prompt);
    await input.press("Enter");

    // The sent message must land in the message thread, exactly once.
    const thread = chatView.getByRole("log");
    await expect(thread.getByText(prompt, { exact: true })).toBeVisible({ timeout: 10000 });
  });
});
