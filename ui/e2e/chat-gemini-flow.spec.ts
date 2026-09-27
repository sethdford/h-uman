import { test, expect } from "@playwright/test";
import { waitForViewReady } from "./helpers";

/**
 * Full chat flow test using demo mode.
 * Uses baseURL from playwright.config (preview server on 4173).
 * Run with: npx playwright test chat-gemini-flow --project=chromium
 */
test.describe("Chat Gemini Flow", () => {
  test("full chat flow - send message and get demo response", async ({ page }) => {
    await page.goto("/?demo#chat");
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 10000 });
    // The view chunk loads lazily; wait for it to upgrade and render the composer.
    await waitForViewReady(page, "hu-chat-view", 10000);

    const prompt = "Hello! What can you do?";
    const textarea = chatView.locator("hu-chat-composer textarea");
    await textarea.fill(prompt);
    await textarea.press("Enter");

    // The thread starts empty in demo mode. The sent message lands as a user
    // bubble, then the demo gateway streams one assistant reply after its
    // simulated latency. Bubbles are role="article", labelled by sender.
    const thread = chatView.locator("hu-message-thread");
    const userBubbles = thread.getByRole("article", { name: /from user|Your message/ });
    const assistantBubbles = thread.getByRole("article", {
      name: /from assistant|Assistant message/,
    });
    await expect(userBubbles).toHaveCount(1, { timeout: 10000 });
    await expect(userBubbles).toContainText(prompt);
    await expect(assistantBubbles).toHaveCount(1, { timeout: 10000 });
    // .content holds the message body only; the timestamp is slotted outside it.
    await expect(assistantBubbles.locator(".content")).toHaveText(/\S/, { timeout: 10000 });
  });
});
