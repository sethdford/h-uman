import { test, expect, type Page } from "@playwright/test";
import { waitForViewReady } from "./helpers";

/** Whether `tag` is registered. Read only after waitForViewReady: chat-view.js
 *  imports these components statically, so they are defined by the time the
 *  view has upgraded and rendered. */
function isDefined(page: Page, tag: string): Promise<boolean> {
  return page.evaluate((t) => customElements.get(t) !== undefined, tag);
}

test.describe("Chat View", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForLoadState("domcontentloaded");
    await expect(page.locator("hu-app >> hu-chat-view")).toBeAttached({ timeout: 5000 });
    // hu-app renders <hu-chat-view> before the lazy chat-view chunk has loaded,
    // so "attached" can still be an un-upgraded element. Wait until it is
    // defined and has rendered its shadow root.
    await waitForViewReady(page, "hu-chat-view");
  });

  test("chat view renders", async ({ page }) => {
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("chat input is visible and focusable", async ({ page }) => {
    const chatView = page.locator("hu-app >> hu-chat-view");
    const input = chatView.locator("textarea, input[type='text'], [contenteditable]").first();
    await expect(input).toBeVisible({ timeout: 5000 });
    const disabled = await input.getAttribute("disabled");
    if (disabled === null) {
      await input.focus();
      await expect(input).toBeFocused();
    }
  });

  test("composer is visible with input and send", async ({ page }) => {
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    const input = chatView.locator("textarea, input[type='text'], [contenteditable]").first();
    await expect(input).toBeVisible({ timeout: 5000 });
    const sendBtn = chatView.getByRole("button", { name: "Send" });
    await expect(sendBtn).toBeVisible({ timeout: 5000 });
  });

  test("typing in chat input works", async ({ page }) => {
    const chatView = page.locator("hu-app >> hu-chat-view");
    const input = chatView.locator("textarea, input[type='text'], [contenteditable]").first();
    await expect(input).toBeVisible({ timeout: 5000 });
    const disabled = await input.getAttribute("disabled");
    if (disabled === null) {
      await input.fill("Hello h-uman");
      await expect(input).toHaveValue("Hello h-uman");
    }
  });

  test("chat view has proper ARIA structure", async ({ page }) => {
    const chatView = page.locator("hu-app >> hu-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    // Message log lives inside hu-message-thread shadow (role="log")
    const messagesArea = page.locator(
      "hu-app >> hu-chat-view >> hu-message-thread >> [role='log']",
    );
    await expect(messagesArea).toBeAttached({ timeout: 5000 });
  });

  test("hu-message-thread component is available", async ({ page }) => {
    expect(
      await isDefined(page, "hu-message-thread"),
      "hu-message-thread not registered after hu-chat-view rendered",
    ).toBe(true);
  });

  test("hu-thinking component is available", async ({ page }) => {
    expect(
      await isDefined(page, "hu-thinking"),
      "hu-thinking not registered after hu-chat-view rendered",
    ).toBe(true);
  });

  test("hu-tool-result component is available", async ({ page }) => {
    expect(
      await isDefined(page, "hu-tool-result"),
      "hu-tool-result not registered after hu-chat-view rendered",
    ).toBe(true);
  });

  test("keyboard shortcut focuses input", async ({ page }) => {
    // Slash key should focus the chat input
    await page.keyboard.press("/");
    await expect(async () => {
      const focused = await page.evaluate(() => {
        const active = document.activeElement;
        if (!active) return null;
        const shadow = active.shadowRoot;
        if (!shadow) return active.tagName;
        const inner = shadow.activeElement;
        return inner?.tagName ?? active.tagName;
      });
      expect(focused).toBeTruthy();
    }).toPass({ timeout: 3000 });
  });
});
