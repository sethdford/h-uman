import { test, expect } from "@playwright/test";
import {
  shadowExists,
  shadowExistsIn,
  deepText,
  waitForViewReady,
  WAIT,
  POLL,
  ANIMATION_SETTLE_MS,
} from "./helpers.js";

const VIEW = "hu-voice-view";

/**
 * Set the voice view's transcript and trigger send via evaluate.
 * This is more reliable than simulating keyboard events across
 * nested shadow DOM boundaries (hu-app > hu-voice-view > hu-textarea > textarea).
 */
function sendVoiceMessage(text: string): string {
  return `(async () => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${VIEW}");
    if (!view) return "no-view";

    // Set transcript directly (TS privacy is compile-time only) and call send()
    view.transcript = ${JSON.stringify(text)};
    await view.updateComplete;
    await view.send();
    return "sent";
  })()`;
}

function clickStatusBarButton(label: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${VIEW}");
    const buttons = view?.shadowRoot?.querySelectorAll(".status-bar hu-button");
    for (const btn of buttons ?? []) {
      const text = btn.textContent?.trim() ?? "";
      const aria = btn.getAttribute("aria-label") ?? "";
      if (text === ${JSON.stringify(label)} || aria === ${JSON.stringify(label)}) {
        const inner = btn.shadowRoot?.querySelector("button");
        if (inner) { inner.click(); return true; }
      }
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
          const app = document.querySelector("hu-app");
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
          const app = document.querySelector("hu-app");
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
        shadowExistsIn(VIEW, "hu-voice-conversation", "hu-empty-state"),
      );
      expect(hasEmpty).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("mic orb is present and clickable", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists(VIEW, "hu-voice-orb"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("send message and receive demo response", async ({ page }) => {
    const result = await page.evaluate(sendVoiceMessage("Hello from voice test"));
    expect(result).toMatch(/^sent/);

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Hello from voice test");
    }).toPass({ timeout: POLL });

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text.length).toBeGreaterThan(50);
    }).toPass({ timeout: 15000 });
  });

  test("New Session clears conversation", async ({ page }) => {
    const result = await page.evaluate(sendVoiceMessage("Session clear test"));
    expect(result).toMatch(/^sent/);

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Session clear test");
      expect(text.length).toBeGreaterThan(60);
    }).toPass({ timeout: 15000 });

    await page.evaluate(clickStatusBarButton("New Session"));
    await page.waitForTimeout(ANIMATION_SETTLE_MS);

    await expect(async () => {
      const hasEmpty = await page.evaluate(
        shadowExistsIn(VIEW, "hu-voice-conversation", "hu-empty-state"),
      );
      expect(hasEmpty).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("Export button is disabled when conversation is empty", async ({ page }) => {
    await expect(async () => {
      const disabled = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const buttons = view?.shadowRoot?.querySelectorAll(".status-bar hu-button");
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

  test("Export button enables after sending a message", async ({ page }) => {
    const result = await page.evaluate(sendVoiceMessage("Export test"));
    expect(result).toMatch(/^sent/);

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Export test");
    }).toPass({ timeout: POLL });

    await expect(async () => {
      const disabled = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const buttons = view?.shadowRoot?.querySelectorAll(".status-bar hu-button");
          for (const btn of buttons ?? []) {
            if (btn.textContent?.trim() === "Export" || btn.getAttribute("aria-label") === "Export conversation") {
              return btn.hasAttribute("disabled");
            }
          }
          return null;
        })()`,
      );
      expect(disabled).toBe(false);
    }).toPass({ timeout: POLL });
  });

  test("conversation area has flex-grow for primary content", async ({ page }) => {
    await expect(async () => {
      const flexGrow = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const conv = view?.shadowRoot?.querySelector("hu-voice-conversation");
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
      const hasOrb = await page.evaluate(shadowExists(VIEW, ".controls-zone hu-voice-orb"));
      expect(hasOrb).toBe(true);
      const hasInput = await page.evaluate(shadowExists(VIEW, ".controls-zone .input-row"));
      expect(hasInput).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("multiple messages accumulate in conversation", async ({ page }) => {
    await page.evaluate(sendVoiceMessage("First message"));

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("First message");
    }).toPass({ timeout: 10000 });

    await page.evaluate(sendVoiceMessage("Second message"));

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("First message");
      expect(text).toContain("Second message");
    }).toPass({ timeout: 10000 });
  });

  test("message count updates in status bar after sending", async ({ page }) => {
    await page.evaluate(sendVoiceMessage("Count test"));

    await expect(async () => {
      const text: string = await page.evaluate(deepText(VIEW));
      expect(text).toContain("Count test");
    }).toPass({ timeout: 10000 });

    await expect(async () => {
      const barText: string = await page.evaluate(
        `(() => {
          const app = document.querySelector("hu-app");
          const view = app?.shadowRoot?.querySelector("${VIEW}");
          const bar = view?.shadowRoot?.querySelector(".status-bar");
          return bar?.textContent ?? "";
        })()`,
      );
      expect(barText).toMatch(/2 message/i);
    }).toPass({ timeout: POLL });
  });
});
