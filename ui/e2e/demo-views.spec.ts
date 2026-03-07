import { test, expect } from "@playwright/test";
import { shadowCount, shadowExists, shadowElementText, shadowText, WAIT, POLL } from "./helpers.js";

// ─────────────────────────────────────────────────────────────
// Overview View
// ─────────────────────────────────────────────────────────────
test.describe("Overview (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#overview");
    await page.waitForTimeout(WAIT);
  });

  test("shows stat cards", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-overview-view", "sc-stat-card"));
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("shows channel items", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-overview-view", ".channel-item"));
      expect(count).toBeGreaterThanOrEqual(5);
    }).toPass({ timeout: POLL });
  });

  test("shows sessions table", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-overview-view", ".sessions-table"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows live activity timeline", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-overview-view", "sc-timeline"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Chat View
// ─────────────────────────────────────────────────────────────
test.describe("Chat (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForTimeout(WAIT);
  });

  test("shows message thread component", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-message-thread"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows chat composer", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-chat-composer"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows connected status", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-chat-view"));
      expect(text).toContain("Connected");
    }).toPass({ timeout: POLL });
  });

  test("shows sessions panel toggle", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-chat-sessions-panel"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Agents View
// ─────────────────────────────────────────────────────────────
test.describe("Agents (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#agents");
    await page.waitForTimeout(WAIT);
  });

  test("shows 4 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-agents-view", "sc-stat-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("shows sessions data table", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-agents-view", "sc-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has New Chat button", async ({ page }) => {
    await expect(async () => {
      const found = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-agents-view");
        const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("New Chat"));
      })()`);
      expect(found).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows agent config section", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-agents-view", ".profile-grid"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Models View
// ─────────────────────────────────────────────────────────────
test.describe("Models (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#models");
    await page.waitForTimeout(WAIT);
  });

  test("shows 5 provider cards", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-models-view", ".grid sc-card"));
      expect(count).toBe(5);
    }).toPass({ timeout: POLL });
  });

  test("shows default provider and model in info section", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-models-view"));
      expect(text).toContain("Default provider");
      expect(text).toContain("Default model");
    }).toPass({ timeout: POLL });
  });

  test("shows API key status on cards", async ({ page }) => {
    await expect(async () => {
      const hasKeyCount = await page.evaluate(shadowCount("sc-models-view", ".key-status.has"));
      expect(hasKeyCount).toBe(4);
      const missingCount = await page.evaluate(
        shadowCount("sc-models-view", ".key-status.missing"),
      );
      expect(missingCount).toBe(1);
    }).toPass({ timeout: POLL });
  });

  test("shows default badge on openrouter", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowElementText("sc-models-view", ".card-name.default"));
      expect(text).toBe("openrouter");
    }).toPass({ timeout: POLL });
  });

  test("search filters providers", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-models-view", ".grid sc-card"))).toBe(5);
    }).toPass({ timeout: POLL });

    await page.evaluate(`
      (() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-models-view");
        const search = view?.shadowRoot?.querySelector("sc-search");
        search?.dispatchEvent(new CustomEvent("sc-search", { detail: { value: "ollama" } }));
      })()
    `);
    await page.waitForTimeout(500);

    const count = await page.evaluate(shadowCount("sc-models-view", ".grid sc-card"));
    expect(count).toBe(1);
  });
});

// ─────────────────────────────────────────────────────────────
// Tools View
// ─────────────────────────────────────────────────────────────
test.describe("Tools (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#tools");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-tools-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows data table with tools", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-tools-view", "sc-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Channels View
// ─────────────────────────────────────────────────────────────
test.describe("Channels (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#channels");
    await page.waitForTimeout(WAIT);
  });

  test("shows data table with channels", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-channels-view", "sc-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows segmented control filter", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-channels-view", "sc-segmented-control"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-channels-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Automations View
// ─────────────────────────────────────────────────────────────
test.describe("Automations (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#automations");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-automations-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows 4 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-automations-view", "sc-stat-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("shows metric row", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-automations-view", "sc-metric-row"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has New Automation button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-automations-view"));
      expect(text).toContain("New Automation");
    }).toPass({ timeout: POLL });
  });

  test("has tabs component", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-automations-view", "sc-tabs"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows automation cards", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-automations-view", "sc-automation-card"));
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Config View
// ─────────────────────────────────────────────────────────────
test.describe("Config (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#config");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero with section header", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-config-view", "sc-page-hero"))).toBe(true);
      expect(await page.evaluate(shadowExists("sc-config-view", "sc-section-header"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has Save button", async ({ page }) => {
    await expect(async () => {
      const found = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-config-view");
        const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("Save"));
      })()`);
      expect(found).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has Raw JSON toggle", async ({ page }) => {
    await expect(async () => {
      const found = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-config-view");
        const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("Raw JSON") || b.textContent?.includes("Form"));
      })()`);
      expect(found).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows config card", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-config-view", "sc-card"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Usage View
// ─────────────────────────────────────────────────────────────
test.describe("Usage (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#usage");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-usage-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows 3 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-usage-view", "sc-stat-card"))).toBe(3);
    }).toPass({ timeout: POLL });
  });

  test("shows token chart", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-usage-view", "sc-chart"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows cost by provider with 3 rows", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-usage-view", ".provider-row"))).toBe(3);
    }).toPass({ timeout: POLL });
  });

  test("shows export button", async ({ page }) => {
    await expect(async () => {
      const found = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-usage-view");
        const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("Export"));
      })()`);
      expect(found).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Security View
// ─────────────────────────────────────────────────────────────
test.describe("Security (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#security");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-security-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows 4 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-security-view", "sc-stat-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("shows Autonomy Level card", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toContain("Autonomy Level");
    }).toPass({ timeout: POLL });
  });

  test("shows Sandbox card with backend info", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toContain("Sandbox");
    }).toPass({ timeout: POLL });
  });

  test("shows Network Proxy card", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-security-view"));
      expect(text).toContain("Network Proxy");
    }).toPass({ timeout: POLL });
  });

  test("has autonomy level select", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-security-view", "sc-select"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Nodes View
// ─────────────────────────────────────────────────────────────
test.describe("Nodes (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-nodes-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows data table with nodes", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-nodes-view", "sc-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows refresh button", async ({ page }) => {
    await expect(async () => {
      const found = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-nodes-view");
        const btns = view?.shadowRoot?.querySelectorAll("sc-button") ?? [];
        return [...btns].some(b => b.textContent?.includes("Refresh"));
      })()`);
      expect(found).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Logs View
// ─────────────────────────────────────────────────────────────
test.describe("Logs (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#logs");
    await page.waitForTimeout(WAIT);
  });

  test("shows page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-logs-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows log area with content", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-logs-view", ".log-area"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("log area contains timeline or empty state", async ({ page }) => {
    await expect(async () => {
      const hasTimeline = await page.evaluate(shadowExists("sc-logs-view", "sc-timeline"));
      const hasEmpty = await page.evaluate(shadowExists("sc-logs-view", "sc-empty-state"));
      expect(hasTimeline || hasEmpty).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has filter input", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-logs-view", "sc-input"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has clear button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-logs-view"));
      expect(text).toContain("Clear");
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Voice View
// ─────────────────────────────────────────────────────────────
test.describe("Voice (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#voice");
    await page.waitForTimeout(WAIT);
  });

  test("shows Voice hero section", async ({ page }) => {
    await expect(async () => {
      const hasHero = await page.evaluate(shadowExists("sc-voice-view", "sc-page-hero"));
      const hasCompactHero = await page.evaluate(shadowExists("sc-voice-view", ".hero"));
      expect(hasHero || hasCompactHero).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows mic button", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".mic-btn"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has text input area", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".input-bar sc-textarea"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });

  test("has send button", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".input-bar sc-button"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows empty conversation state", async ({ page }) => {
    await expect(async () => {
      const hasEmpty = await page.evaluate(shadowExists("sc-voice-view", ".conversation-empty"));
      const hasEmptyState = await page.evaluate(shadowExists("sc-voice-view", "sc-empty-state"));
      expect(hasEmpty || hasEmptyState).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Skills View (supplements existing tests)
// ─────────────────────────────────────────────────────────────
test.describe("Skills (Demo) — extended", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(WAIT);
  });

  test("shows Install from URL input and button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-skills-view"));
      expect(text).toContain("Install");
    }).toPass({ timeout: POLL });
  });

  test("shows tag chips for filtering", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-skills-view", ".tag-chip"));
      expect(count).toBeGreaterThanOrEqual(5);
    }).toPass({ timeout: POLL });
  });

  test("shows registry cards", async ({ page }) => {
    await expect(async () => {
      const sections = await page.evaluate(shadowCount("sc-skills-view", ".section"));
      expect(sections).toBeGreaterThanOrEqual(2);
    }).toPass({ timeout: POLL });
  });
});
