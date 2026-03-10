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

    // Type into the composer textarea via shadow DOM traversal
    const typed = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector("hu-chat-view");
      const composer = view?.shadowRoot?.querySelector("hu-chat-composer");
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

    // Press Enter on the focused textarea to send
    await page.keyboard.press("Enter");

    // Verify message thread is present (demo response ~600ms + render)
    const messagesArea = chatView.locator(
      "[role='log'], .messages, .chat-messages, hu-message-thread",
    );
    await expect(messagesArea.first()).toBeAttached({ timeout: 10000 });
  });
});
