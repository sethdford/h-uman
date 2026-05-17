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

// ─────────────────────────────────────────────────────────────
// Memory (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Memory (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#memory");
    await page.waitForTimeout(WAIT);
  });

  test("shows memory entries with keys", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-memory-view"));
      expect(text).toContain("user-prefers-dark-mode");
    }).toPass({ timeout: POLL });
  });

  test("shows category badges", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-memory-view"));
      expect(text).toMatch(/core/i);
    }).toPass({ timeout: POLL });
  });

  test("search input accepts text", async ({ page }) => {
    await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector("hu-memory-view");
      const input = view?.shadowRoot?.querySelector("hu-input");
      const nativeInput = input?.shadowRoot?.querySelector("input");
      if (nativeInput) {
        (nativeInput as HTMLInputElement).value = "dark mode";
        nativeInput.dispatchEvent(new Event("input", { bubbles: true }));
      }
    });
    await page.waitForTimeout(600);

    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-memory-view"));
      expect(text).toContain("user-prefers-dark-mode");
    }).toPass({ timeout: POLL });
  });

  test("segmented control filter exists", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-memory-view", "hu-segmented-control"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });

  test("graph section available", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-memory-view"));
      expect(text).toMatch(/graph|knowledge|connection/i);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Channels (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Channels (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#channels");
    await page.waitForTimeout(WAIT);
  });

  test("shows channel list with configured channels", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-channels-view"));
      expect(text).toContain("Telegram");
      expect(text).toContain("Discord");
    }).toPass({ timeout: POLL });
  });

  test("shows channel status", async ({ page }) => {
    // channels-view.statusLabel returns "Active" / "Unconfigured" /
    // "Error" (see channels-view.ts:173). The previous regex used
    // "Connected" / "Not configured" — vocabulary that doesn't exist
    // in the implementation. Match the actual labels.
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-channels-view"));
      expect(text).toMatch(/Active|Unconfigured|Error/);
    }).toPass({ timeout: POLL });
  });

  test("shows unconfigured channels", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-channels-view"));
      expect(text).toContain("Signal");
    }).toPass({ timeout: POLL });
  });

  test("page hero renders", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-channels-view", "hu-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Nodes (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Nodes (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(WAIT);
  });

  test("shows node list with hostnames", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-nodes-view"));
      expect(text).toMatch(/local|remote-prod|staging/);
    }).toPass({ timeout: POLL });
  });

  test("shows node status indicators", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-nodes-view"));
      expect(text).toMatch(/online|offline|degraded/i);
    }).toPass({ timeout: POLL });
  });

  test("shows refresh button", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-nodes-view"));
      expect(text).toContain("Refresh");
    }).toPass({ timeout: POLL });
  });

  test("data table renders", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-nodes-view", "hu-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Usage (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Usage (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#usage");
    await page.waitForTimeout(WAIT);
  });

  test("shows cost summary", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-usage-view"));
      expect(text).toMatch(/\$|cost|token/i);
    }).toPass({ timeout: POLL });
  });

  test("shows provider breakdown", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-usage-view"));
      expect(text).toContain("openrouter");
    }).toPass({ timeout: POLL });
  });

  test("page hero renders", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-usage-view", "hu-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("stat cards present", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("hu-usage-view", "hu-stat-card"));
      expect(count).toBeGreaterThanOrEqual(2);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Logs (Interactions)
// ─────────────────────────────────────────────────────────────
test.describe("Logs (Interactions)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#logs");
    await page.waitForTimeout(WAIT);
  });

  test("shows log output area", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-logs-view", ".log-area"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has filter input", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-logs-view", ".filter-input"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows level filter controls", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("hu-logs-view"));
      expect(text).toMatch(/debug|info|warn|error/i);
    }).toPass({ timeout: POLL });
  });

  test("page hero renders", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("hu-logs-view", "hu-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});
