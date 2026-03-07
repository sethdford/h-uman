import { test, expect } from "@playwright/test";
import {
  shadowClick,
  shadowCount,
  shadowCountIn,
  shadowExists,
  shadowExistsIn,
  shadowText,
  WAIT,
  POLL,
} from "./helpers.js";

// ─────────────────────────────────────────────────────────────
// Chat View (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Chat (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForTimeout(WAIT);
  });

  test("send message and receive demo response", async ({ page }) => {
    // Focus textarea: sc-chat-view → sc-chat-composer → textarea (nested shadow)
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-chat-view");
      const composer = view?.shadowRoot?.querySelector("sc-chat-composer");
      const textarea = composer?.shadowRoot?.querySelector("textarea");
      textarea?.focus();
    });
    await page.keyboard.type("Hello world");

    // Click Send: sc-chat-view → sc-chat-composer → button[aria-label="Send"]
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-chat-view");
      const composer = view?.shadowRoot?.querySelector("sc-chat-composer");
      const btn = composer?.shadowRoot?.querySelector('button[aria-label="Send"]');
      btn?.click();
    });

    // Wait for demo response (600ms + render). Verify view text contains the response.
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-chat-view"));
      expect(text).toContain("Demo response to: Hello world");
    }).toPass({ timeout: POLL });
  });

  test("sessions panel shows sessions", async ({ page }) => {
    // Open sessions panel (toggle)
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-chat-view");
      const toggle = view?.shadowRoot?.querySelector('[aria-label="Open sessions"]');
      toggle?.click();
    });
    await page.waitForTimeout(400);

    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-chat-sessions-panel"))).toBe(
        true,
      );
      const text = await page.evaluate(shadowText("sc-chat-view"));
      expect(text).toContain("Project Planning");
    }).toPass({ timeout: POLL });
  });

  test("connected status shows", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-chat-view"));
      expect(text).toContain("Connected");
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Config View (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Config (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#config");
    await page.waitForTimeout(WAIT);
  });

  test("shows save button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-config-view"));
      expect(text).toContain("Save");
    }).toPass({ timeout: POLL });
  });

  test("raw JSON toggle works", async ({ page }) => {
    // Find and click "Raw JSON" button
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-config-view");
      const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
      const rawBtn = [...btns].find((b) => b.textContent?.includes("Raw JSON"));
      rawBtn?.click();
    });
    await page.waitForTimeout(400);

    // Verify raw JSON view: textarea or code block with JSON content
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-config-view"));
      expect(text).toMatch(/"provider"|"openrouter"/);
    }).toPass({ timeout: POLL });

    // Click again to switch back to Form
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-config-view");
      const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
      const formBtn = [...btns].find((b) => b.textContent?.includes("Form"));
      formBtn?.click();
    });
    await page.waitForTimeout(400);

    // Verify form view restored (has form groups)
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-config-view", "sc-form-group"))).toBe(true);
      const text = await page.evaluate(shadowText("sc-config-view"));
      expect(text).toContain("Save");
    }).toPass({ timeout: POLL });
  });

  test("page hero and section headers render", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-config-view", "sc-page-hero"))).toBe(true);
      const count = await page.evaluate(shadowCount("sc-config-view", "sc-section-header"));
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Security View (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Security (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#security");
    await page.waitForTimeout(WAIT);
  });

  test("shows autonomy level section", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toContain("Autonomy");
    }).toPass({ timeout: POLL });
  });

  test("shows sandbox section", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toContain("Sandbox");
    }).toPass({ timeout: POLL });
  });

  test("shows network proxy section", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toMatch(/Network|Proxy/);
    }).toPass({ timeout: POLL });
  });

  test("has select for autonomy level", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-security-view", "sc-select"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});
