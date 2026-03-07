import { test, expect } from "@playwright/test";

/**
 * Full chat flow test using demo mode.
 * Uses baseURL from playwright.config (preview server on 4173).
 * Run with: npx playwright test chat-gemini-flow --project=chromium
 */
test.describe("Chat Gemini Flow", () => {
  test("full chat flow - send message and get demo response", async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForLoadState("networkidle");
    await page.waitForTimeout(1500);

    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 10000 });

    // Type into the composer textarea via shadow DOM traversal
    const typed = await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-chat-view");
      const composer = view?.shadowRoot?.querySelector("sc-chat-composer");
      const textarea = composer?.shadowRoot?.querySelector(
        "textarea",
      ) as HTMLTextAreaElement | null;
      if (!textarea) return false;
      textarea.focus();
      textarea.value = "Hello! What can you do?";
      textarea.dispatchEvent(new Event("input", { bubbles: true }));
      return true;
    });
    expect(typed).toBe(true);

    await page.waitForTimeout(300);

    // Press Enter on the focused textarea to send
    await page.keyboard.press("Enter");
    await page.waitForTimeout(3000);

    // Verify message thread is present
    const messagesArea = chatView.locator(
      "[role='log'], .messages, .chat-messages, sc-message-thread",
    );
    await expect(messagesArea.first()).toBeAttached({ timeout: 10000 });
  });
});
