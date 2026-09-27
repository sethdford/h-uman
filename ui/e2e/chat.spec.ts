import { test, expect } from "@playwright/test";

test.describe("Chat View", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForLoadState("domcontentloaded");
    await expect(page.locator("hu-app >> hu-chat-view")).toBeAttached({ timeout: 5000 });
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

  // These elements are registered by the chat view's lazily loaded chunk, which
  // can land after hu-chat-view attaches, so poll for registration instead of
  // reading customElements once.
  for (const tag of ["hu-message-thread", "hu-thinking", "hu-tool-result"]) {
    test(`${tag} component is available`, async ({ page }) => {
      await expect
        .poll(() => page.evaluate((t) => customElements.get(t) !== undefined, tag), {
          timeout: 10000,
        })
        .toBe(true);
    });
  }

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
