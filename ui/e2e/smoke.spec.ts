import { test, expect } from "@playwright/test";
import {
  waitForViewReady,
  deepText,
  shadowText,
  shadowExists,
  VIEW_TAGS,
  isKnownPageError,
} from "./helpers";

/**
 * E2E Smoke Tests — validates full stack (C gateway → WebSocket → UI).
 *
 * Requires the gateway to be running with `control_ui_dir` serving the built UI.
 * Use playwright-smoke.config.ts which points at the gateway URL.
 *
 * In CI: gateway is started before these tests run (no AI provider needed).
 * Locally: start with `./build/human gateway --with-agent`
 */

test.describe("Smoke: Gateway Health", () => {
  test("health endpoint returns ok", async ({ request }) => {
    const resp = await request.get("/health");
    expect(resp.ok()).toBeTruthy();
    const body = await resp.json();
    expect(body.status).toBe("ok");
    expect(body.version).toBeTruthy();
    expect(body.pid).toBeGreaterThan(0);
  });

  test("status endpoint shows websocket enabled", async ({ request }) => {
    const resp = await request.get("/api/status");
    expect(resp.ok()).toBeTruthy();
    const body = await resp.json();
    expect(body.status).toBe("ok");
    expect(body.websocket).toBe(true);
  });

  test("models endpoint returns provider list", async ({ request }) => {
    const resp = await request.get("/v1/models");
    expect(resp.ok()).toBeTruthy();
    const body = await resp.json();
    expect(body.object).toBe("list");
    expect(body.data).toBeDefined();
    expect(Array.isArray(body.data)).toBe(true);
  });
});

test.describe("Smoke: UI Loads", () => {
  test("dashboard loads from gateway static serving", async ({ page }) => {
    await page.goto("/");
    await expect(page).toHaveTitle(/h-uman Control/);
    await expect(page.locator("hu-app")).toBeAttached({ timeout: 5000 });
  });

  test("WebSocket connects (not demo fallback)", async ({ page }) => {
    await page.goto("/");
    await page.waitForLoadState("domcontentloaded");

    await expect(async () => {
      const status = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        if (!app?.shadowRoot) return "";
        function collectText(root: ShadowRoot | Element): string {
          let text = "";
          const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
          while (walker.nextNode()) text += walker.currentNode.textContent;
          for (const el of root.querySelectorAll("*")) {
            if ((el as any).shadowRoot) text += collectText((el as any).shadowRoot);
          }
          return text;
        }
        return collectText(app.shadowRoot).toLowerCase();
      });
      expect(status).toContain("connected");
    }).toPass({ timeout: 15000 });
  });

  test("no ?demo query param in URL", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(4000);

    const isDemoMode = await page.evaluate(() => {
      return new URL(window.location.href).searchParams.has("demo");
    });
    expect(isDemoMode).toBe(false);
  });
});

test.describe("Smoke: Views Render", () => {
  test("overview loads", async ({ page }) => {
    await page.goto("/#overview");
    await waitForViewReady(page, VIEW_TAGS.overview, 15000);

    await expect(async () => {
      const text = await page.evaluate(deepText(VIEW_TAGS.overview));
      expect(text.toLowerCase()).toContain("h-uman");
    }).toPass({ timeout: 15000 });
  });

  test("models view renders", async ({ page }) => {
    await page.goto("/#models");
    await waitForViewReady(page, VIEW_TAGS.models, 15000);

    await expect(async () => {
      const text = await page.evaluate(deepText(VIEW_TAGS.models));
      expect(text.length).toBeGreaterThan(20);
    }).toPass({ timeout: 15000 });
  });

  test("tools view renders with tool names", async ({ page }) => {
    await page.goto("/#tools");
    await waitForViewReady(page, VIEW_TAGS.tools, 15000);

    await expect(async () => {
      const text = await page.evaluate(deepText(VIEW_TAGS.tools));
      expect(text.length).toBeGreaterThan(20);
    }).toPass({ timeout: 15000 });
  });

  test("config view renders", async ({ page }) => {
    await page.goto("/#config");
    await waitForViewReady(page, VIEW_TAGS.config, 15000);
    const text = await page.evaluate(shadowText(VIEW_TAGS.config));
    expect(text.length).toBeGreaterThan(0);
  });

  test("chat view loads with composer", async ({ page }) => {
    await page.goto("/#chat");
    await waitForViewReady(page, VIEW_TAGS.chat, 15000);

    await expect(async () => {
      const hasComposer = await page.evaluate(
        shadowExists(VIEW_TAGS.chat, "textarea, hu-chat-composer"),
      );
      expect(hasComposer).toBe(true);
    }).toPass({ timeout: 15000 });
  });

  test("channels view renders", async ({ page }) => {
    await page.goto("/#channels");
    await waitForViewReady(page, VIEW_TAGS.channels, 15000);
    const text = await page.evaluate(shadowText(VIEW_TAGS.channels));
    expect(text.length).toBeGreaterThan(0);
  });

  test("logs view renders", async ({ page }) => {
    await page.goto("/#logs");
    await waitForViewReady(page, VIEW_TAGS.logs, 15000);
    const text = await page.evaluate(shadowText(VIEW_TAGS.logs));
    expect(text.length).toBeGreaterThan(0);
  });
});

test.describe("Smoke: Navigation", () => {
  const criticalViews = [
    "overview",
    "chat",
    "models",
    "tools",
    "config",
    "channels",
    "automations",
    "skills",
    "logs",
  ];

  for (const view of criticalViews) {
    test(`#${view} renders its view tag`, async ({ page }) => {
      await page.goto(`/#${view}`);
      await waitForViewReady(page, VIEW_TAGS[view], 15000);
      const exists = await page.evaluate(
        `!!document.querySelector("hu-app")?.shadowRoot?.querySelector("${VIEW_TAGS[view]}")`,
      );
      expect(exists).toBe(true);
    });
  }
});

test.describe("Smoke: No Errors", () => {
  test("no console errors on critical views", async ({ page }) => {
    const errors: string[] = [];
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        const text = msg.text();
        if (
          text.includes("favicon") ||
          text.includes("manifest") ||
          text.includes("ERR_CONNECTION_REFUSED") ||
          text.includes("net::ERR") ||
          text.includes("Content Security Policy") ||
          text.includes("content-security-policy")
        )
          return;
        errors.push(text);
      }
    });

    const views = ["overview", "chat", "models", "tools", "config"];
    for (const view of views) {
      await page.goto(`/#${view}`);
      await waitForViewReady(page, VIEW_TAGS[view], 15000);
    }

    expect(errors).toEqual([]);
  });

  test("no unhandled JS exceptions", async ({ page }) => {
    const exceptions: string[] = [];
    page.on("pageerror", (err) => {
      if (!isKnownPageError(err.message)) exceptions.push(err.message);
    });

    await page.goto("/");
    await page.waitForTimeout(4000);
    await page.goto("/#chat");
    await page.waitForTimeout(2000);
    await page.goto("/#models");
    await page.waitForTimeout(2000);

    expect(exceptions).toEqual([]);
  });
});
