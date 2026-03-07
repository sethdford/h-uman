import { test } from "@playwright/test";

/**
 * Test sending a message via gateway client directly (bypasses Shadow DOM input).
 * Uses baseURL from playwright.config (preview server on 4173).
 * Requires: live gateway on ws:// — skips automatically if disconnected.
 */
test.describe("Chat via Gateway Direct", () => {
  test("send message via gw.request and capture response", async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForLoadState("networkidle");
    await page.waitForTimeout(2000);

    const statusBefore = await page.evaluate(() => {
      const app = document.querySelector("sc-app") as { gateway?: { status: string } } | null;
      return app?.gateway?.status ?? "no-gateway";
    });
    if (statusBefore !== "connected") {
      test.skip(true, `Gateway not connected (${statusBefore}), skipping live gateway test`);
    }
    console.log("Gateway status before:", statusBefore);

    // 3. Send message via gateway client
    const sendResult = await page.evaluate(async () => {
      const app = document.querySelector("sc-app") as {
        gateway?: { status: string; request: (m: string, p: object) => Promise<unknown> };
      } | null;
      const gw = app?.gateway;
      if (!gw || gw.status !== "connected") {
        return { ok: false, error: `Gateway not connected: ${gw?.status ?? "null"}` };
      }
      try {
        await gw.request("chat.send", {
          message: "Hello! What is your name and what can you do? Reply in 2 sentences.",
          sessionKey: "default",
        });
        return { ok: true };
      } catch (e) {
        return { ok: false, error: String(e) };
      }
    });
    console.log("Send result:", sendResult);

    // 4. Wait 15 seconds for Gemini response
    await page.waitForTimeout(15000);

    // 5. Screenshot
    await page.screenshot({
      path: "test-results/chat-gateway-direct-response.png",
      fullPage: true,
    });

    // 6. Check for errors
    const pageText = await page.evaluate(() => document.body.innerText);
    const hasError = pageText.includes("Retry last message") || pageText.includes("error");
    const hasAssistant =
      pageText.includes("assistant") || pageText.includes("SeaClaw") || pageText.includes("Gemini");

    // 7. Try interacting with chat input (skip if no chat view, e.g. demo mode)
    const chatView = page.locator("sc-app >> sc-chat-view");
    const input = chatView.locator("textarea").first();
    const inputVisible = await input.isVisible().catch(() => false);
    if (inputVisible) {
      await input.click();
      await input.fill("Test from Playwright");
      const sendBtn = chatView.getByRole("button", { name: "Send" });
      if (await sendBtn.isVisible().catch(() => false)) {
        await sendBtn.click();
      } else {
        await input.press("Enter");
      }
      await page.waitForTimeout(3000);
      await page.screenshot({
        path: "test-results/chat-input-interaction.png",
        fullPage: true,
      });
    }

    console.log("Has error indicator:", hasError);
    console.log("Has assistant content:", hasAssistant);
  });
});
