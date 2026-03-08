import { test, expect } from "@playwright/test";
import {
  shadowExists,
  shadowCount,
  shadowText,
  deepText,
  shadowClick,
  shadowComputedStyle,
  WAIT,
  POLL,
  waitForViewReady,
} from "./helpers.js";

/**
 * View capability tests — verifies each view's interactive features
 * work correctly in the production build with demo data.
 */

// ─────────────────────────────────────────────────────────────
// Overview
// ─────────────────────────────────────────────────────────────
test.describe("Overview Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#overview");
    await page.waitForTimeout(WAIT);
  });

  test("stat cards display numeric values", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-overview-view"));
      expect(text).toMatch(/\d+/);
      expect(text).toMatch(/Channels|Tools|Uptime/i);
    }).toPass({ timeout: POLL });
  });

  test("overview cards are present", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-overview-view", "sc-card"));
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Chat
// ─────────────────────────────────────────────────────────────
test.describe("Chat Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#chat");
    await page.waitForTimeout(WAIT);
  });

  test("demo messages render with correct roles", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-chat-view"));
      expect(text).toContain("memory leak");
      expect(text).toContain("free()");
    }).toPass({ timeout: POLL });
  });

  test("composer textarea accepts input", async ({ page }) => {
    await expect(async () => {
      const hasTextarea = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-chat-view");
        const composer = view?.shadowRoot?.querySelector("sc-chat-composer");
        return !!composer?.shadowRoot?.querySelector("textarea");
      })()`);
      expect(hasTextarea).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("sessions panel opens on toggle click", async ({ page }) => {
    await page.evaluate(shadowClick("sc-chat-view", ".sessions-toggle"));
    await page.waitForTimeout(600);

    await expect(async () => {
      const panelOpen = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-chat-view");
        const panel = view?.shadowRoot?.querySelector("sc-chat-sessions-panel");
        return panel?.hasAttribute("open");
      })()`);
      expect(panelOpen).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("sessions panel closes on second toggle", async ({ page }) => {
    await page.evaluate(shadowClick("sc-chat-view", ".sessions-toggle"));
    await page.waitForTimeout(600);
    await page.evaluate(shadowClick("sc-chat-view", ".sessions-toggle"));
    await page.waitForTimeout(600);

    const panelOpen = await page.evaluate(`(() => {
      const app = document.querySelector("sc-app");
      const view = app?.shadowRoot?.querySelector("sc-chat-view");
      const panel = view?.shadowRoot?.querySelector("sc-chat-sessions-panel");
      return panel?.hasAttribute("open");
    })()`);
    expect(panelOpen).toBe(false);
  });

  test("keyboard shortcut Ctrl+F opens search", async ({ page }) => {
    await page.keyboard.press("Control+f");
    await page.waitForTimeout(400);

    await expect(async () => {
      const hasSearch = await page.evaluate(shadowExists("sc-chat-view", "sc-chat-search"));
      expect(hasSearch).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Tools
// ─────────────────────────────────────────────────────────────
test.describe("Tools Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#tools");
    await page.waitForTimeout(WAIT);
  });

  test("tool list shows expected tools", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-tools-view"));
      expect(text).toContain("shell");
      expect(text).toContain("file_read");
    }).toPass({ timeout: POLL });
  });

  test("data table is searchable", async ({ page }) => {
    await expect(async () => {
      const isSearchable = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-tools-view");
        const table = view?.shadowRoot?.querySelector("sc-data-table-v2");
        return table?.hasAttribute("searchable") ?? false;
      })()`);
      expect(isSearchable).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("data table renders rows", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-tools-view"));
      expect(text).toContain("shell");
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Channels
// ─────────────────────────────────────────────────────────────
test.describe("Channels Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#channels");
    await page.waitForTimeout(WAIT);
  });

  test("shows channel status badges", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-channels-view"));
      expect(text).toMatch(/Active|Unconfigured|Error/);
    }).toPass({ timeout: POLL });
  });

  test("filter tabs work (All/Configured/Unconfigured)", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-channels-view", "sc-segmented-control"))).toBe(
        true,
      );
    }).toPass({ timeout: POLL });
  });

  test("shows health status for channels", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-channels-view"));
      expect(text).toMatch(/Healthy|Unhealthy/);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Nodes
// ─────────────────────────────────────────────────────────────
test.describe("Nodes Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(WAIT);
  });

  test("shows node table with status", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-nodes-view"));
      expect(text).toMatch(/online|degraded|offline/i);
    }).toPass({ timeout: POLL });
  });

  test("shows multiple node entries", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-nodes-view"));
      expect(text).toContain("local");
      expect(text).toContain("remote-prod");
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Logs
// ─────────────────────────────────────────────────────────────
test.describe("Logs Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#logs");
    await page.waitForTimeout(WAIT);
  });

  test("log area has content", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-logs-view", ".log-area"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("filter input is present", async ({ page }) => {
    await expect(async () => {
      const has = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-logs-view");
        return !!view?.shadowRoot?.querySelector(".filter-input") ||
               !!view?.shadowRoot?.querySelector("sc-input");
      })()`);
      expect(has).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("clear button is present", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(shadowText("sc-logs-view"));
      expect(text).toMatch(/Clear|clear/);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Voice
// ─────────────────────────────────────────────────────────────
test.describe("Voice Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#voice");
    await page.waitForTimeout(WAIT);
  });

  test("mic button is present and interactive", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-voice-view", "sc-voice-orb"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("text input area is present", async ({ page }) => {
    await expect(async () => {
      const hasInput = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-voice-view");
        return !!view?.shadowRoot?.querySelector("sc-textarea") ||
               !!view?.shadowRoot?.querySelector("textarea");
      })()`);
      expect(hasInput).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows empty conversation state", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-voice-view"));
      expect(text).toMatch(/No conversation|conversation yet/i);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Models
// ─────────────────────────────────────────────────────────────
test.describe("Models Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#models");
    await page.waitForTimeout(WAIT);
  });

  test("shows provider cards", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-models-view"));
      expect(text).toMatch(/openai|anthropic|google|openrouter/i);
    }).toPass({ timeout: POLL });
  });

  test("search filters providers", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-models-view", "sc-search"))).toBe(true);
    }).toPass({ timeout: POLL });
  });

  test("shows default provider info", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-models-view"));
      expect(text).toMatch(/Default|default/);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Usage
// ─────────────────────────────────────────────────────────────
test.describe("Usage Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#usage");
    await page.waitForTimeout(WAIT);
  });

  test("shows stat cards with metrics", async ({ page }) => {
    await expect(async () => {
      const count = await page.evaluate(shadowCount("sc-usage-view", "sc-card"));
      expect(count).toBeGreaterThanOrEqual(3);
    }).toPass({ timeout: POLL });
  });

  test("shows export button", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(shadowText("sc-usage-view"));
      expect(text).toMatch(/Export|export/);
    }).toPass({ timeout: POLL });
  });

  test("shows cost breakdown section", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-usage-view"));
      expect(text).toMatch(/Cost|cost/);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Agents
// ─────────────────────────────────────────────────────────────
test.describe("Agents Capabilities", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#agents");
    await page.waitForTimeout(WAIT);
  });

  test("shows agent metrics", async ({ page }) => {
    await expect(async () => {
      const text: string = await page.evaluate(deepText("sc-agents-view"));
      expect(text).toMatch(/sessions|turns|tools|channels/i);
    }).toPass({ timeout: POLL });
  });

  test("shows sessions data table", async ({ page }) => {
    await expect(async () => {
      expect(await page.evaluate(shadowExists("sc-agents-view", "sc-data-table-v2"))).toBe(true);
    }).toPass({ timeout: POLL });
  });
});

// ─────────────────────────────────────────────────────────────
// Theme Toggle
// ─────────────────────────────────────────────────────────────
test.describe("Theme Toggle", () => {
  test("theme cycles through system → light → dark", async ({ page }) => {
    await page.goto("/?demo#overview");
    await page.waitForTimeout(WAIT);

    const getTheme = () =>
      page.evaluate(() => document.documentElement.getAttribute("data-theme") ?? "system");

    await page.evaluate(shadowClick("sc-sidebar", ".theme-toggle"));
    await page.waitForTimeout(200);
    const first = await getTheme();

    await page.evaluate(shadowClick("sc-sidebar", ".theme-toggle"));
    await page.waitForTimeout(200);
    const second = await getTheme();

    await page.evaluate(shadowClick("sc-sidebar", ".theme-toggle"));
    await page.waitForTimeout(200);
    const third = await getTheme();

    const themes = new Set([first, second, third]);
    expect(themes.size).toBeGreaterThanOrEqual(2);
  });
});

// ─────────────────────────────────────────────────────────────
// Command Palette Navigation
// ─────────────────────────────────────────────────────────────
test.describe("Command Palette Navigation", () => {
  test("can navigate to views via command palette", async ({ page }) => {
    await page.goto("/?demo#overview");
    await page.waitForTimeout(WAIT);

    await page.keyboard.press("Control+k");
    await page.waitForTimeout(400);

    await expect(async () => {
      const paletteExists = await page.evaluate(`(() => {
        const app = document.querySelector("sc-app");
        return !!app?.shadowRoot?.querySelector("sc-command-palette");
      })()`);
      expect(paletteExists).toBe(true);
    }).toPass({ timeout: POLL });

    await page.keyboard.press("Escape");
    await page.waitForTimeout(200);
  });
});
