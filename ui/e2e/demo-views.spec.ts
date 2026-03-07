import { test, expect } from "@playwright/test";

/**
 * Helper: query inside a view's shadow DOM via page.evaluate.
 * Returns the count of elements matching `selector` inside the given view tag.
 */
function shadowCount(viewTag: string, selector: string) {
  return `
    (() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("${viewTag}");
      return view?.shadowRoot?.querySelectorAll("${selector}").length ?? 0;
    })()
  `;
}

function shadowExists(viewTag: string, selector: string) {
  return `
    (() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("${viewTag}");
      return !!view?.shadowRoot?.querySelector("${selector}");
    })()
  `;
}

function shadowText(viewTag: string, selector: string) {
  return `
    (() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("${viewTag}");
      const el = view?.shadowRoot?.querySelector("${selector}");
      return el?.textContent?.trim() ?? "";
    })()
  `;
}

function shadowClick(viewTag: string, selector: string) {
  return `
    (() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("${viewTag}");
      const el = view?.shadowRoot?.querySelector("${selector}");
      el?.click();
      return !!el;
    })()
  `;
}

const WAIT = 1800;
const POLL = 8000;

// ─────────────────────────────────────────────────────────────
// Overview View
// ─────────────────────────────────────────────────────────────
test.describe("Overview (Demo)", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#overview");
    await page.waitForTimeout(WAIT);
  });

  test("renders page hero", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-overview-view", "sc-page-hero"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows 4 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-overview-view", "sc-stat-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("shows channel items", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-overview-view", ".channel-item"));
      expect(count).toBeGreaterThanOrEqual(5);
    }).toPass({ timeout: POLL });
  });

  test("shows sessions table with rows", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-overview-view", ".sessions-table"))).toBe(true);
      const rows = await page.evaluate(shadowCount("sc-overview-view", ".sessions-table tbody tr"));
      expect(rows).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("shows live activity timeline", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-overview-view", "sc-timeline"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("refresh button exists", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-overview-view", ".hero-actions sc-button"));
      expect(text).toContain("Refresh");
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

  test("shows message stream component", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-message-stream"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows composer with input and send button", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-chat-view", "sc-composer"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows connected status", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-chat-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Connected");
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

  test("shows session rows", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-agents-view", ".session-row"));
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("has New Chat button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-agents-view");
          const btns = [...(view?.shadowRoot?.querySelectorAll("sc-button") ?? [])];
          return btns.map(b => b.textContent?.trim()).join("|");
        })()
      `);
      expect(text).toContain("New Chat");
    }).toPass({ timeout: POLL });
  });

  test("has Resume buttons on session rows", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-agents-view");
          const btns = [...(view?.shadowRoot?.querySelectorAll(".session-row sc-button") ?? [])];
          return btns.filter(b => b.textContent?.trim() === "Resume").length;
        })()
      `);
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("shows agent profile section", async ({ page }) => {
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

  test("shows default provider and model in info bar", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(shadowText("sc-models-view", ".info-bar"));
      expect(text).toContain("openrouter");
      expect(text).toContain("claude-sonnet-4");
    }).toPass({ timeout: POLL });
  });

  test("shows API key status on cards", async ({ page }) => {
    await expect(async () => {
      const hasKeyCount = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-models-view");
          return view?.shadowRoot?.querySelectorAll(".key-status.has").length ?? 0;
        })()
      `);
      expect(hasKeyCount).toBe(4);
      const missingCount = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-models-view");
          return view?.shadowRoot?.querySelectorAll(".key-status.missing").length ?? 0;
        })()
      `);
      expect(missingCount).toBe(1);
    }).toPass({ timeout: POLL });
  });

  test("shows default badge on openrouter", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-models-view");
          const badge = view?.shadowRoot?.querySelector(".card-name.default");
          return badge?.textContent?.trim() ?? "";
        })()
      `);
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

  test("shows 2 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-tools-view", "sc-stat-card"))).toBe(2);
    }).toPass({ timeout: POLL });
  });

  test("shows tool cards in grid", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-tools-view", ".grid sc-card"));
      expect(count).toBeGreaterThanOrEqual(6);
    }).toPass({ timeout: POLL });
  });

  test("each tool card has name and description", async ({ page }) => {
    await expect(async () => {
      const names = await page.evaluate(shadowCount("sc-tools-view", ".card-name"));
      const descs = await page.evaluate(shadowCount("sc-tools-view", ".card-desc"));
      expect(names).toBeGreaterThanOrEqual(6);
      expect(descs).toBeGreaterThanOrEqual(6);
    }).toPass({ timeout: POLL });
  });

  test("search input exists", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-tools-view", ".search sc-input"))).toBe(true);
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

  test("shows 3 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-channels-view", "sc-stat-card"))).toBe(3);
    }).toPass({ timeout: POLL });
  });

  test("shows channel cards", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-channels-view", ".channel-card"));
      expect(count).toBe(8);
    }).toPass({ timeout: POLL });
  });

  test("channel cards show status badges", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-channels-view", ".channel-card sc-badge"));
      expect(count).toBeGreaterThanOrEqual(6);
    }).toPass({ timeout: POLL });
  });

  test("search filters channels", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-channels-view", ".channel-card"))).toBe(8);
    }).toPass({ timeout: POLL });

    await page.evaluate(`
      (() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-channels-view");
        const search = view?.shadowRoot?.querySelector("sc-search");
        search?.dispatchEvent(new CustomEvent("sc-search", { detail: { value: "telegram" } }));
      })()
    `);
    await page.waitForTimeout(500);

    const count = await page.evaluate(shadowCount("sc-channels-view", ".channel-card"));
    expect(count).toBe(1);
  });

  test("shows status indicators", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-channels-view", ".status-indicator"));
      expect(count).toBeGreaterThanOrEqual(6);
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
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-automations-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("New Automation");
    }).toPass({ timeout: POLL });
  });

  test("has tabs for Agent Tasks and Shell Jobs", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-automations-view", "sc-tabs"))).toBe(true);
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-automations-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Agent Tasks");
      expect(text).toContain("Shell Jobs");
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

  test("shows page hero with Configuration heading", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-config-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Configuration");
    }).toPass({ timeout: POLL });
  });

  test("has Save button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-config-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Save");
    }).toPass({ timeout: POLL });
  });

  test("has Raw JSON toggle", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-config-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Raw JSON");
    }).toPass({ timeout: POLL });
  });

  test("shows General section", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-config-view", ".section"))).toBe(true);
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-config-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("General");
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

  test("shows cost cards with dollar values", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-usage-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("$0.42");
      expect(text).toContain("$3.87");
      expect(text).toContain("$18.42");
    }).toPass({ timeout: POLL });
  });

  test("shows token sparkline chart", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-usage-view", "sc-sparkline-enhanced"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });

  test("shows provider bar chart", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-usage-view", ".bar-chart"))).toBe(true);
      const barCount = await page.evaluate(shadowCount("sc-usage-view", ".bar-row"));
      expect(barCount).toBe(3);
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

  test("shows 3 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-security-view", "sc-stat-card"))).toBe(3);
    }).toPass({ timeout: POLL });
  });

  test("shows Autonomy Level card", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-security-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Autonomy Level");
      expect(text).toContain("Supervised");
    }).toPass({ timeout: POLL });
  });

  test("shows Sandbox card with backend info", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-security-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Sandbox");
      expect(text).toContain("landlock");
    }).toPass({ timeout: POLL });
  });

  test("shows Network Proxy card with allowed domains", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-security-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Network Proxy");
      expect(text).toContain("api.openai.com");
      expect(text).toContain("api.anthropic.com");
    }).toPass({ timeout: POLL });
  });

  test("has autonomy level select", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-security-view", "#autonomy-select"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows security checklist", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-security-view", ".check-item"));
      expect(count).toBeGreaterThanOrEqual(3);
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

  test("shows 4 stat cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-nodes-view", "sc-stat-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("shows 4 node cards", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-nodes-view", ".node-card"))).toBe(4);
    }).toPass({ timeout: POLL });
  });

  test("search filters nodes", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-nodes-view", ".node-card"))).toBe(4);
    }).toPass({ timeout: POLL });

    const input = page.locator("sc-app >> sc-nodes-view").locator("sc-input");
    await input.locator("input").fill("rpi");
    await page.waitForTimeout(500);
    expect(await page.evaluate(shadowCount("sc-nodes-view", ".node-card"))).toBe(1);
  });

  test("detail sheet opens on card click", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-nodes-view", ".node-card"))).toBe(4);
    }).toPass({ timeout: POLL });

    await page.evaluate(shadowClick("sc-nodes-view", ".node-card"));
    await page.waitForTimeout(500);
    expect(await page.evaluate(shadowExists("sc-nodes-view", "sc-sheet"))).toBe(true);
    expect(await page.evaluate(shadowExists("sc-nodes-view", ".detail-name"))).toBe(true);
  });

  test("detail sheet has action buttons", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowCount("sc-nodes-view", ".node-card"))).toBe(4);
    }).toPass({ timeout: POLL });

    await page.evaluate(shadowClick("sc-nodes-view", ".node-card"));
    await page.waitForTimeout(500);
    const btnCount = await page.evaluate(shadowCount("sc-nodes-view", ".detail-actions sc-button"));
    expect(btnCount).toBe(2);
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

  test("shows log entries from seed events", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-logs-view", ".log-line"));
      expect(count).toBeGreaterThanOrEqual(4);
    }).toPass({ timeout: POLL });
  });

  test("log entries have timestamps and event types", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-logs-view");
          const lines = view?.shadowRoot?.querySelectorAll(".log-line");
          return [...(lines ?? [])].map(l => l.textContent).join("|");
        })()
      `);
      expect(text).toContain("[chat]");
      expect(text).toContain("[tool_call]");
    }).toPass({ timeout: POLL });
  });

  test("has filter input", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-logs-view", "sc-input"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has clear button", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-logs-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
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

  test("shows Voice Assistant hero", async ({ page }) => {
    await expect(async () => {
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-voice-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Voice Assistant");
      expect(text).toContain("Connected");
    }).toPass({ timeout: POLL });
  });

  test("shows mic button", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".mic-btn"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has text input area", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".input-bar textarea"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("has send button", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".send-btn"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows empty conversation state", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", ".empty-conversation"))).toBe(true);
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
      const text = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-skills-view");
          return view?.shadowRoot?.textContent ?? "";
        })()
      `);
      expect(text).toContain("Install");
    }).toPass({ timeout: POLL });
  });

  test("shows tag chips for filtering", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-skills-view");
          return view?.shadowRoot?.querySelectorAll(".tag-chip").length ?? 0;
        })()
      `);
      expect(count).toBeGreaterThanOrEqual(5);
    }).toPass({ timeout: POLL });
  });

  test("shows registry cards", async ({ page }) => {
    await expect(async () => {
      const sections = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          const view = app?.shadowRoot?.querySelector("sc-skills-view");
          return view?.shadowRoot?.querySelectorAll(".section").length ?? 0;
        })()
      `);
      expect(sections).toBeGreaterThanOrEqual(2);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Cross-view Navigation
// ─────────────────────────────────────────────────────────────
test.describe("Navigation (Demo)", () => {
  test("sidebar navigation works through all views", async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForTimeout(WAIT);

    const views = [
      "chat",
      "agents",
      "models",
      "tools",
      "channels",
      "skills",
      "automations",
      "config",
      "voice",
      "nodes",
      "usage",
      "security",
      "logs",
    ];

    for (const view of views) {
      await page.evaluate((v) => (window.location.hash = v), view);
      await page.waitForTimeout(600);
      const tag = `sc-${view}-view`;
      const exists = await page.evaluate(`
        (() => {
          const app = document.querySelector("sc-app");
          return !!app?.shadowRoot?.querySelector("${tag}");
        })()
      `);
      expect(exists).toBe(true);
    }
  });

  test("Ctrl+K opens command palette in demo mode", async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForTimeout(WAIT);
    await page.keyboard.press("Control+k");
    await page.waitForTimeout(500);
    const exists = await page.evaluate(`
      (() => {
        const app = document.querySelector("sc-app");
        return !!app?.shadowRoot?.querySelector("sc-command-palette");
      })()
    `);
    expect(exists).toBe(true);
  });
});
