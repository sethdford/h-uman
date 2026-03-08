import { test, expect } from "@playwright/test";
import {
  shadowExists,
  shadowExistsIn,
  shadowClick,
  shadowText,
  deepText,
  waitForViewReady,
  WAIT,
  POLL,
} from "./helpers.js";

const VIEW = "sc-voice-view";

/**
 * Type text into the voice view's textarea via shadow DOM traversal,
 * then return whether it succeeded.
 */
function typeInVoiceInput(text: string): string {
  return `(async () => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${VIEW}");
    const textarea = view?.shadowRoot?.querySelector("sc-textarea");
    const inner = textarea?.shadowRoot?.querySelector("textarea");
    if (!inner) return false;
    inner.focus();
    inner.value = ${JSON.stringify(text)};
    inner.dispatchEvent(new Event("input", { bubbles: true }));
    return true;
  })()`;
}

function clickSendButton(): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${VIEW}");
    const buttons = view?.shadowRoot?.querySelectorAll(".input-row sc-button");
    for (const btn of buttons ?? []) {
      const inner = btn.shadowRoot?.querySelector("button");
      if (inner && !inner.disabled) { inner.click(); return true; }
    }
    return false;
  })()`;
}

test.describe("Voice Interactions", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#voice");
    await waitForViewReady(page, VIEW);
    await page.waitForTimeout(WAIT);
  });

  test("renders status bar with connection info", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, ".status-bar"))).toBe(true);
      const text: string = await page.evaluate(
        `(() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const bar = view?.shadowRoot?.querySelector(".status-bar");
          return bar?.textContent ?? "";
        })()`,
      );
      expect(text).toMatch(/Voice/i);
      expect(text).toMatch(/Connected|Disconnected|Reconnecting/i);
    }).toPass({ timeout: POLL });
  });

  test("status bar has New Session and Export buttons", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(
        `(() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const bar = view?.shadowRoot?.querySelector(".status-bar");
          return bar?.textContent ?? "";
        })()`,
      );
      expect(text).toContain("New Session");
      expect(text).toContain("Export");
    }).toPass({ timeout: POLL });
  });

  test("shows empty conversation state on fresh load", async ({ page }) => {
    await expect(async () => {
      const hasEmpty = await page.evaluate(
        shadowExistsIn(VIEW, "sc-voice-conversation", "sc-empty-state"),
      );
      expect(hasEmpty).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("mic orb is present and clickable", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "sc-voice-orb"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("send message via text input and receive demo response", async ({ page }) => {
    const typed = await page.evaluate(typeInVoiceInput("Hello from voice test"));
    expect(typed).toBe(true);

    await page.keyboard.press("Enter");

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Hello from voice test");
    }).toPass({ timeout: POLL });

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Demo response to: Hello from voice test");
    }).toPass({ timeout: 10000 });
  });

  test("send button triggers message", async ({ page }) => {
    const typed = await page.evaluate(typeInVoiceInput("Button send test"));
    expect(typed).toBe(true);

    const clicked = await page.evaluate(clickSendButton());
    expect(clicked).toBe(true);

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Button send test");
    }).toPass({ timeout: POLL });

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Demo response to: Button send test");
    }).toPass({ timeout: 10000 });
  });

  test("New Session clears conversation", async ({ page }) => {
    const typed = await page.evaluate(typeInVoiceInput("Session clear test"));
    expect(typed).toBe(true);
    await page.keyboard.press("Enter");

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Session clear test");
    }).toPass({ timeout: POLL });

    await page.evaluate(
      `(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("${VIEW}");
        const buttons = view?.shadowRoot?.querySelectorAll(".status-bar sc-button");
        for (const btn of buttons ?? []) {
          if (btn.textContent?.trim() === "New Session" || btn.getAttribute("aria-label") === "Start new session") {
            btn.shadowRoot?.querySelector("button")?.click();
            return true;
          }
        }
        return false;
      })()`,
    );

    await expect(async () => {
      const hasEmpty = await page.evaluate(
        shadowExistsIn(VIEW, "sc-voice-conversation", "sc-empty-state"),
      );
      expect(hasEmpty).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("Export button is disabled when conversation is empty", async ({ page }) => {
    await expect(async () => {
      const disabled = await page.evaluate(
        `(() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const buttons = view?.shadowRoot?.querySelectorAll(".status-bar sc-button");
          for (const btn of buttons ?? []) {
            if (btn.textContent?.trim() === "Export" || btn.getAttribute("aria-label") === "Export conversation") {
              return btn.hasAttribute("disabled");
            }
          }
          return null;
        })()`,
      );
      expect(disabled).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("conversation area has flex-grow for primary content", async ({ page }) => {
    await expect(async () => {
      const flexGrow = await page.evaluate(
        `(() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const conv = view?.shadowRoot?.querySelector("sc-voice-conversation");
          if (!conv) return "0";
          return getComputedStyle(conv).flexGrow;
        })()`,
      );
      expect(Number(flexGrow)).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: POLL });
  });

  test("controls zone is anchored below conversation", async ({ page }) => {
    await expect(async () => {
      const exists = await page.evaluate(shadowExists(VIEW, ".controls-zone"));
      expect(exists).toBe(true);
      const hasOrb = await page.evaluate(shadowExists(VIEW, ".controls-zone sc-voice-orb"));
      expect(hasOrb).toBe(true);
      const hasInput = await page.evaluate(shadowExists(VIEW, ".controls-zone .input-row"));
      expect(hasInput).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("Enter sends message, Shift+Enter does not", async ({ page }) => {
    const typed = await page.evaluate(typeInVoiceInput("Shift enter test"));
    expect(typed).toBe(true);

    await page.keyboard.press("Shift+Enter");
    await page.waitForTimeout(300);

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).not.toContain("Demo response to: Shift enter test");
    }).toPass({ timeout: 3000 });

    await page.evaluate(typeInVoiceInput("Enter send test"));
    await page.keyboard.press("Enter");

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Enter send test");
    }).toPass({ timeout: POLL });
  });

  test("multiple messages accumulate in conversation", async ({ page }) => {
    await page.evaluate(typeInVoiceInput("First message"));
    await page.keyboard.press("Enter");

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Demo response to: First message");
    }).toPass({ timeout: 10000 });

    await page.evaluate(typeInVoiceInput("Second message"));
    await page.keyboard.press("Enter");

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("First message");
      expect(text).toContain("Second message");
    }).toPass({ timeout: 10000 });
  });
});
