import { test, expect, type Page } from "@playwright/test";
import { waitForViewReady, POLL } from "./helpers.js";

/** Visible markdown text length inside the last assistant hu-chat-bubble (shadow-piercing). */
async function getAssistantBubbleVisibleLength(page: Page): Promise<number> {
  return page.evaluate(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("hu-chat-view");
    const thread = view?.shadowRoot?.querySelector("hu-message-thread");
    const bubbles = thread?.shadowRoot?.querySelectorAll("hu-chat-bubble") ?? [];
    for (let i = bubbles.length - 1; i >= 0; i--) {
      const b = bubbles[i]!;
      const label =
        b.shadowRoot?.querySelector('[role="article"]')?.getAttribute("aria-label") ?? "";
      if (label.includes("assistant")) {
        return (b.shadowRoot?.querySelector(".content")?.textContent ?? "").trim().length;
      }
    }
    return 0;
  });
}

async function sendChatMessage(page: Page, text: string): Promise<void> {
  const chatView = page.locator("hu-app >> hu-chat-view");
  await expect(page.locator("hu-app >> hu-chat-view >> .status-bar")).toContainText("Connected", {
    timeout: 25000,
  });
  const input = chatView.locator("#composer-textarea").or(chatView.locator("textarea")).first();
  await expect(input).toBeVisible({ timeout: 8000 });
  await expect(input).toBeEnabled({ timeout: 8000 });
  await input.fill(text);
  const sendBtn = chatView.getByRole("button", { name: "Send" });
  await expect(sendBtn).toBeEnabled({ timeout: 8000 });
  await sendBtn.click();
}

function thread(page: Page) {
  return page.locator("hu-app >> hu-chat-view >> hu-message-thread");
}

/** Innermost focused element (walks shadow roots). */
async function deepActiveTagName(page: Page): Promise<string> {
  return page.evaluate(() => {
    let el: Element | null = document.activeElement;
    while (el?.shadowRoot?.activeElement) {
      el = el.shadowRoot.activeElement;
    }
    return el?.tagName.toLowerCase() ?? "";
  });
}

/** True if deep focus is the tool card "Input" section toggle. */
async function deepActiveIsToolInputToggle(page: Page): Promise<boolean> {
  return page.evaluate(() => {
    let el: Element | null = document.activeElement;
    while (el?.shadowRoot?.activeElement) {
      el = el.shadowRoot.activeElement;
    }
    if (!el || el.tagName.toLowerCase() !== "button") return false;
    const t = el.textContent?.replace(/\s+/g, " ").trim() ?? "";
    return t.startsWith("Input");
  });
}

test.describe("Chat streaming UI (demo mode)", () => {
  test.describe.configure({ timeout: 60_000 });

  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      try {
        for (const k of Object.keys(sessionStorage)) {
          if (k.startsWith("hu-chat-")) sessionStorage.removeItem(k);
        }
      } catch {
        /* ignore */
      }
    });
    await page.goto("/?demo#chat");
    await waitForViewReady(page, "hu-chat-view");
  });

  test("test_streaming_shows_reasoning_block — hu-reasoning-block visible and expanded while streaming", async ({
    page,
  }) => {
    await sendChatMessage(page, `reasoning stream ${Date.now()}`);

    const reasoning = thread(page).locator("hu-reasoning-block").last();
    await expect(reasoning).toBeVisible({ timeout: POLL });

    const region = reasoning.locator('[role="region"]').first();
    await expect(region).toBeVisible();
    await expect(region).toHaveAttribute("aria-label", "AI reasoning");

    const toggle = reasoning.getByRole("button", { name: "Toggle reasoning content" });
    await expect(toggle).toBeVisible();
    await expect(async () => {
      const expanded = await toggle.getAttribute("aria-expanded");
      expect(expanded).toBe("true");
    }).toPass({ timeout: 12000 });
  });

  test("test_streaming_shows_tool_call_card — tool name, Running… then Completed", async ({ page }) => {
    await sendChatMessage(page, `tool stream ${Date.now()}`);

    const toolCard = thread(page).locator("hu-tool-result").last();
    await expect(toolCard).toBeVisible({ timeout: POLL });

    await expect(toolCard.locator(".tool-name")).toBeVisible();
    await expect(async () => {
      const name = await toolCard.locator(".tool-name").textContent();
      expect(name === "web_search" || name === "file_read").toBe(true);
    }).toPass({ timeout: 5000 });

    await expect(toolCard.locator(".subtitle")).toHaveText(/Running\.\.\./i, { timeout: 12000 });
    await expect(toolCard.locator(".subtitle")).toHaveText("Completed", { timeout: 20000 });
  });

  test("test_streaming_text_appears_progressively — assistant bubble content grows during stream", async ({
    page,
  }) => {
    await sendChatMessage(page, `progressive text ${Date.now()}`);

    const assistantBubble = thread(page).locator("hu-chat-bubble").last();
    await expect(async () => {
      const label = await assistantBubble.locator('[role="article"]').getAttribute("aria-label");
      expect(label ?? "").toMatch(/assistant/i);
    }).toPass({ timeout: POLL });
    await expect(assistantBubble).toBeVisible();

    await expect(async () => {
      const samples: number[] = [];
      for (let i = 0; i < 10; i++) {
        samples.push(await getAssistantBubbleVisibleLength(page));
        await page.waitForTimeout(140);
      }
      const grew = samples.some((s, idx) => idx > 0 && s > samples[idx - 1]!);
      expect(grew).toBe(true);
    }).toPass({ timeout: 20000 });
  });

  test("test_reasoning_block_collapses_after_streaming — auto-collapse after long content", async ({
    page,
  }) => {
    await sendChatMessage(page, `collapse reasoning ${Date.now()}`);

    const toggle = thread(page)
      .locator("hu-reasoning-block")
      .last()
      .getByRole("button", { name: "Toggle reasoning content" });

    await expect(toggle).toBeVisible({ timeout: POLL });
    await expect(async () => {
      expect(await toggle.getAttribute("aria-expanded")).toBe("true");
    }).toPass({ timeout: 12000 });

    await expect(async () => {
      expect(await toggle.getAttribute("aria-expanded")).toBe("false");
    }).toPass({ timeout: 8000 });
  });

  test("test_tool_card_shows_input_and_result — expand Input; Output shows tool result text", async ({
    page,
  }) => {
    await sendChatMessage(page, `tool sections ${Date.now()}`);

    const toolCard = thread(page).locator("hu-tool-result").last();
    await expect(toolCard).toBeVisible({ timeout: POLL });

    await expect(async () => {
      expect((await toolCard.locator(".subtitle").textContent())?.trim()).toBe("Completed");
    }).toPass({ timeout: POLL });

    const inputToggle = toolCard.getByRole("button", { name: /^Input$/ });
    await expect(inputToggle).toBeVisible();
    await expect(inputToggle).toHaveAttribute("aria-expanded", "false");
    await inputToggle.scrollIntoViewIfNeeded();
    /* Hit-testing can be intercepted by main/suggestion layers; invoke DOM click on the real button node. */
    await inputToggle.evaluate((el) => (el as HTMLButtonElement).click());
    await expect(inputToggle).toHaveAttribute("aria-expanded", "true");
    await expect(toolCard.locator("pre.code-block")).toBeVisible();

    const outputToggle = toolCard.getByRole("button", { name: /^Output$/ });
    await expect(outputToggle).toBeVisible();
    await expect(outputToggle).toHaveAttribute("aria-expanded", "true");
    await expect(toolCard.locator(".code-block").filter({ hasText: /Demo tool output/i })).toBeVisible();
  });

  test("test_streaming_accessibility — region role, aria-expanded, Tab moves focus", async ({
    page,
  }) => {
    await sendChatMessage(page, `a11y stream ${Date.now()}`);

    const threadLoc = thread(page);
    const reasoning = threadLoc.locator("hu-reasoning-block").last();
    await expect(reasoning).toBeVisible({ timeout: POLL });
    await expect(reasoning.locator('[role="region"][aria-label="AI reasoning"]')).toBeVisible();

    const toolCard = threadLoc.locator("hu-tool-result").last();
    await expect(async () => {
      expect((await toolCard.locator(".subtitle").textContent())?.trim()).toBe("Completed");
    }).toPass({ timeout: POLL });

    /* With JSON input, header is non-interactive region (expand/collapse is per-section). */
    await expect(toolCard.locator(".header")).toHaveAttribute("role", "region");

    const inputToggle = toolCard.getByRole("button", { name: /^Input$/ });
    await expect(inputToggle).toHaveAttribute("aria-expanded", "false");
    const outputToggle = toolCard.getByRole("button", { name: /^Output$/ });
    await expect(outputToggle).toHaveAttribute("aria-expanded", "true");

    const reasoningToggle = reasoning.getByRole("button", { name: "Toggle reasoning content" });
    reasoningToggle.focus();
    await expect(reasoningToggle).toBeFocused();

    await page.keyboard.press("Tab");
    await expect(async () => {
      expect(["button", "textarea", "input"]).toContain(await deepActiveTagName(page));
    }).toPass({ timeout: 5000 });

    /* Tab through thread until tool Input toggle is focused (keyboard operable chain). */
    let reachedInput = await deepActiveIsToolInputToggle(page);
    for (let i = 0; i < 14 && !reachedInput; i++) {
      await page.keyboard.press("Tab");
      reachedInput = await deepActiveIsToolInputToggle(page);
    }
    expect(reachedInput).toBe(true);
    await expect(inputToggle).toBeFocused();
  });
});
