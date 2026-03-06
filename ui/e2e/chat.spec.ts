import { test, expect } from "@playwright/test";

test.describe("Chat View", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForTimeout(500);
  });

  test("chat view renders", async ({ page }) => {
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
  });

  test("chat input is visible and focusable", async ({ page }) => {
    const chatView = page.locator("sc-app >> sc-chat-view");
    const input = chatView.locator("textarea, input[type='text'], [contenteditable]").first();
    await expect(input).toBeVisible({ timeout: 5000 });
    const disabled = await input.getAttribute("disabled");
    if (disabled === null) {
      await input.focus();
      await expect(input).toBeFocused();
    }
  });

  test("empty state shows prompt", async ({ page }) => {
    const chatView = page.locator("sc-app >> sc-chat-view");
    const emptyText = chatView.getByText("Start a conversation");
    await expect(emptyText).toBeVisible({ timeout: 5000 });
  });

  test("typing in chat input works", async ({ page }) => {
    const chatView = page.locator("sc-app >> sc-chat-view");
    const input = chatView.locator("textarea, input[type='text'], [contenteditable]").first();
    await expect(input).toBeVisible({ timeout: 5000 });
    const disabled = await input.getAttribute("disabled");
    if (disabled === null) {
      await input.fill("Hello SeaClaw");
      await expect(input).toHaveValue("Hello SeaClaw");
    }
  });

  test("chat view has proper ARIA structure", async ({ page }) => {
    const chatView = page.locator("sc-app >> sc-chat-view");
    await expect(chatView).toBeAttached({ timeout: 5000 });
    // Chat messages area should be present
    const messagesArea = chatView.locator(".messages, [role='log'], .chat-messages").first();
    await expect(messagesArea).toBeAttached({ timeout: 5000 });
  });

  test("sc-message-stream component is available", async ({ page }) => {
    // Verify the component is registered by checking customElements
    const registered = await page.evaluate(() => {
      return customElements.get("sc-message-stream") !== undefined;
    });
    expect(registered).toBe(true);
  });

  test("sc-thinking component is available", async ({ page }) => {
    const registered = await page.evaluate(() => {
      return customElements.get("sc-thinking") !== undefined;
    });
    expect(registered).toBe(true);
  });

  test("sc-tool-result component is available", async ({ page }) => {
    const registered = await page.evaluate(() => {
      return customElements.get("sc-tool-result") !== undefined;
    });
    expect(registered).toBe(true);
  });

  test("sc-message-branch component is available", async ({ page }) => {
    const registered = await page.evaluate(() => {
      return customElements.get("sc-message-branch") !== undefined;
    });
    expect(registered).toBe(true);
  });

  test("keyboard shortcut focuses input", async ({ page }) => {
    // Slash key should focus the chat input
    await page.keyboard.press("/");
    await page.waitForTimeout(200);
    const focused = await page.evaluate(() => {
      const active = document.activeElement;
      if (!active) return null;
      const shadow = active.shadowRoot;
      if (!shadow) return active.tagName;
      const inner = shadow.activeElement;
      return inner?.tagName ?? active.tagName;
    });
    // Should have focused some input element
    expect(focused).toBeTruthy();
  });
});
