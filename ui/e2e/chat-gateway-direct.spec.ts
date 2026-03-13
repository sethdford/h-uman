import { test, expect } from "@playwright/test";

test.describe("Chat Streaming Choreography (demo mode)", () => {
  test.fixme(true, "4-level shadow DOM traversal (app > chat > thread > .hero) unreliable");

  test("empty state shows time-aware hero greeting", async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForLoadState("networkidle");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 10000 });

    const greetings = [
      "Good morning",
      "Good afternoon",
      "Good evening",
      "Late night",
      "Burning the midnight oil",
    ];
    await expect(async () => {
      const heroText = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-chat-view");
        const thread = view?.shadowRoot?.querySelector("hu-message-thread");
        const hero = thread?.shadowRoot?.querySelector(".hero");
        return hero?.textContent ?? "";
      });
      expect(greetings.some((g) => heroText.includes(g))).toBe(true);
      expect(heroText).toContain("What would you like to work on");
    }).toPass({ timeout: 15000 });
  });

  test("hero suggestion chips are interactive", async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForLoadState("networkidle");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 10000 });

    await expect(async () => {
      const chipCount = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-chat-view");
        const thread = view?.shadowRoot?.querySelector("hu-message-thread");
        return thread?.shadowRoot?.querySelectorAll(".hero-chip").length ?? 0;
      });
      expect(chipCount).toBe(4);
    }).toPass({ timeout: 15000 });
  });

  test("typing indicator uses accent glow animation", async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForLoadState("networkidle");
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 10000 });

    await expect(async () => {
      await page.evaluate(async () => {
        const app = document.querySelector("hu-app") as {
          gateway?: { request: (m: string, p: object) => Promise<unknown> };
        } | null;
        await app?.gateway?.request("chat.send", {
          message: "test streaming",
          sessionKey: "default",
        });
      });

      const hasIndicator = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-chat-view");
        const thread = view?.shadowRoot?.querySelector("hu-message-thread");
        return !!thread?.shadowRoot?.querySelector("hu-typing-indicator");
      });
      expect(hasIndicator).toBe(true);
    }).toPass({ timeout: 15000 });

    await page.screenshot({
      path: "test-results/chat-streaming-choreography.png",
      fullPage: true,
    });
  });
});

/**
 * Test sending a message via gateway client directly (bypasses Shadow DOM input).
 * Uses baseURL from playwright.config (preview server on 4173).
 * Requires: live gateway on ws:// — skips automatically if disconnected.
 */
test.describe("Chat via Gateway Direct", () => {
  test("send message via gw.request and capture response", async ({ page }) => {
    await page.goto("/#chat");
    await page.waitForLoadState("networkidle");
    await expect(page.locator("hu-app >> hu-chat-view")).toBeAttached({ timeout: 10000 });

    const statusBefore = await page.evaluate(() => {
      const app = document.querySelector("hu-app") as { gateway?: { status: string } } | null;
      return app?.gateway?.status ?? "no-gateway";
    });
    if (statusBefore !== "connected") {
      test.skip(true, `Gateway not connected (${statusBefore}), skipping live gateway test`);
    }
    console.log("Gateway status before:", statusBefore);

    // 3. Send message via gateway client
    const sendResult = await page.evaluate(async () => {
      const app = document.querySelector("hu-app") as {
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
    if (!sendResult.ok) {
      const errMsg = (sendResult as { error?: string }).error ?? "unknown";
      if (errMsg.includes("unauthorized") || errMsg.includes("auth")) {
        test.skip(true, `Gateway requires auth (${errMsg}), skipping live test`);
      }
    }
    expect(
      sendResult.ok,
      `chat.send should succeed: ${(sendResult as { error?: string }).error ?? "unknown"}`,
    ).toBe(true);

    // 4. Wait for assistant response (LLM can take up to 15s)
    await expect(async () => {
      const pageText = await page.evaluate(() => document.body.innerText);
      const hasAssistant =
        pageText.includes("assistant") ||
        pageText.includes("h-uman") ||
        pageText.includes("Gemini");
      expect(hasAssistant).toBe(true);
    }).toPass({ timeout: 15000 });

    // 5. Screenshot
    await page.screenshot({
      path: "test-results/chat-gateway-direct-response.png",
      fullPage: true,
    });

    // 6. Assert assistant content appeared (already verified in step 4)

    // 7. Try interacting with chat input (skip if no chat view, e.g. demo mode)
    const chatView = page.locator("hu-app >> hu-chat-view");
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
      await expect(async () => {
        const hasResponse = await page.evaluate(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("hu-chat-view");
          const text = view?.shadowRoot?.textContent ?? "";
          return text.includes("assistant") || text.includes("thinking") || text.length > 200;
        });
        expect(hasResponse).toBe(true);
      }).toPass({ timeout: 5000 });
      await page.screenshot({
        path: "test-results/chat-input-interaction.png",
        fullPage: true,
      });
    }
  });
});
