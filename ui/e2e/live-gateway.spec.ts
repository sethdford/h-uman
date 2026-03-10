import { test, expect } from "@playwright/test";

const LIVE = process.env.HUMAN_LIVE_E2E === "1";

test.describe("Live Gateway E2E", () => {
  test.skip(!LIVE, "Set HUMAN_LIVE_E2E=1 to run live gateway tests");

  test.beforeEach(async ({ page }) => {
    await page.goto("/#chat");
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 10000 });
  });

  test("gateway connection established", async ({ page }) => {
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    const statusDot = chatView.locator(".status-dot.connected");
    await expect(statusDot).toBeAttached({ timeout: 15000 });

    const statusText = chatView.getByText("Connected");
    await expect(statusText).toBeVisible({ timeout: 5000 });

    const textarea = chatView.locator("textarea");
    await expect(textarea).toBeEnabled({ timeout: 5000 });
  });

  test("send message and receive AI response", async ({ page }) => {
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    await expect(chatView.locator(".status-dot.connected")).toBeAttached({
      timeout: 15000,
    });

    const textarea = chatView.locator("textarea");
    await textarea.fill("What is 2+2? Reply with just the number.");
    const sendBtn = chatView.locator(".send-btn");
    await sendBtn.click();

    const assistantMsg = chatView
      .locator("hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble")
      .first();
    await expect(assistantMsg).toBeVisible({ timeout: 30000 });

    const content = await assistantMsg.evaluate((el) => {
      const stream = el.querySelector("hu-message-stream");
      const streamText = stream?.shadowRoot?.querySelector(".content")?.textContent ?? "";
      const bubbleText =
        (el as HTMLElement).shadowRoot?.querySelector(".content")?.textContent ?? "";
      return streamText || bubbleText || (el as HTMLElement).textContent || "";
    });
    expect(content).toContain("4");
  });

  test("streaming indicator appears during response", async ({ page }) => {
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    await expect(chatView.locator(".status-dot.connected")).toBeAttached({
      timeout: 15000,
    });

    const textarea = chatView.locator("textarea");
    await textarea.fill("Count from 1 to 10 slowly, one number per line.");
    const sendBtn = chatView.locator(".send-btn");
    await sendBtn.click();

    const thinkingOrStream = chatView.locator(
      "hu-thinking, hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble",
    );
    await expect(thinkingOrStream.first()).toBeVisible({ timeout: 15000 });

    const assistantMsg = chatView
      .locator("hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble")
      .first();
    await expect(assistantMsg).toBeVisible({ timeout: 30000 });
  });

  test("multi-turn conversation maintains context", async ({ page }) => {
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    await expect(chatView.locator(".status-dot.connected")).toBeAttached({
      timeout: 15000,
    });

    const textarea = chatView.locator("textarea");
    const sendBtn = chatView.locator(".send-btn");

    await textarea.fill("Remember this number: 42. Reply with just OK.");
    await sendBtn.click();
    const firstReply = chatView
      .locator("hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble")
      .first();
    await expect(firstReply).toBeVisible({ timeout: 30000 });

    await textarea.fill("What was the number I just told you? Reply with just the number.");
    await sendBtn.click();

    const replies = chatView.locator(
      "hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble",
    );
    await expect(async () => {
      const count = await replies.count();
      expect(count).toBeGreaterThanOrEqual(2);
    }).toPass({ timeout: 30000 });
    const count = await replies.count();
    const lastReply = replies.nth(count - 1);
    await expect(lastReply).toBeVisible({ timeout: 30000 });

    const content = await lastReply.evaluate((el) => {
      const stream = el.querySelector("hu-message-stream");
      const streamText = stream?.shadowRoot?.querySelector(".content")?.textContent ?? "";
      const bubbleText =
        (el as HTMLElement).shadowRoot?.querySelector(".content")?.textContent ?? "";
      return streamText || bubbleText || (el as HTMLElement).textContent || "";
    });
    expect(content).toContain("42");
  });

  test("connected chat screenshot", async ({ page }) => {
    const chatView = page.locator("hu-app").locator("hu-chat-view");
    await expect(chatView.locator(".status-dot.connected")).toBeAttached({
      timeout: 15000,
    });

    const textarea = chatView.locator("textarea");
    await textarea.fill("Say hello in one sentence.");
    const sendBtn = chatView.locator(".send-btn");
    await sendBtn.click();

    const assistantMsg = chatView
      .locator("hu-message-thread hu-message-group[role='assistant'] hu-chat-bubble")
      .first();
    await expect(assistantMsg).toBeVisible({ timeout: 30000 });

    await page.screenshot({
      path: "e2e-results/live-gateway-chat.png",
      fullPage: true,
    });
  });
});
