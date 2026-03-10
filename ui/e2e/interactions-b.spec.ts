import { test, expect } from "@playwright/test";
import { shadowCount, shadowExists, shadowText, deepText, WAIT, POLL } from "./helpers.js";

// ─────────────────────────────────────────────────────────────
// Automations (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Automations (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#automations");
    await page.waitForTimeout(WAIT);
  });

  test("shows existing automations", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-automations-view"));
      expect(text).toContain("Daily Summary");
    }).toPass({ timeout: POLL });
  });

  test("new automation button exists", async ({ page }) => {
    await expect(async () => {
      const hasBtn = await page.evaluate(`(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-automations-view");
        const btns = view?.shadowRoot?.querySelectorAll("hu-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("New"));
      })()`);
      expect(hasBtn).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("paused automation shows paused state", async ({ page }) => {
    await expect(async () => {
      const hasPausedCard = await page.evaluate(`(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-automations-view");
        const cards = view?.shadowRoot?.querySelectorAll("hu-automation-card") ?? [];
        for (const card of cards) {
          const wrapper = card.shadowRoot?.querySelector(".card-wrapper.paused");
          const nameEl = card.shadowRoot?.querySelector(".job-name");
          if (wrapper && nameEl?.textContent?.includes("Daily Standup")) return true;
        }
        return false;
      })()`);
      expect(hasPausedCard).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shell jobs tab shows shell automations", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-automations-view", "hu-tabs"))).toBe(true);
    }).toPass({ timeout: POLL });

    await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector("hu-automations-view");
      const tabs = view?.shadowRoot?.querySelector("hu-tabs");
      const shellTab = tabs?.shadowRoot?.querySelector(
        '[data-tab-id="shell"]',
      ) as HTMLElement | null;
      shellTab?.click();
    });
    await page.waitForTimeout(600);

    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-automations-view"));
      expect(text).toContain("Health Check");
    }).toPass({ timeout: POLL });
  });

  test("automation card shows type badge", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-automations-view"));
      expect(text).toMatch(/Agent|Shell/);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Skills (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Skills (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(WAIT);
  });

  test("shows installed skills", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("hu-skills-view", "hu-skill-card"));
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: POLL });
  });

  test("shows registry section", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-skills-view", "hu-skill-registry"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("page hero renders", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-skills-view", "hu-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});
