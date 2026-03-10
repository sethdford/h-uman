import { test, expect } from "@playwright/test";
import { ANIMATION_SETTLE_MS } from "./helpers.js";

/**
 * Sidebar collapse/expand transition tests.
 *
 * Verifies that the sidebar smoothly transitions between collapsed and
 * expanded states without layout snapping. Covers the min-width transition
 * fix and grid-template-columns animation.
 */
test.describe("Sidebar Transitions", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForLoadState("domcontentloaded");
    await page.locator("hu-app >> .layout").waitFor({ timeout: 5000 });
  });

  function getSidebarMetrics() {
    return `(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      if (!sidebar) return null;
      const host = sidebar;
      const style = getComputedStyle(host);
      const rect = host.getBoundingClientRect();
      return {
        width: rect.width,
        minWidth: parseFloat(style.minWidth) || 0,
        hasCollapsed: host.hasAttribute("collapsed"),
        transition: style.transition,
      };
    })()`;
  }

  test("sidebar starts expanded with correct width", async ({ page }) => {
    const metrics = await page.evaluate(getSidebarMetrics());
    expect(metrics).not.toBeNull();
    expect(metrics.hasCollapsed).toBe(false);
    expect(metrics.width).toBeGreaterThan(200);
  });

  test("Ctrl+B collapses sidebar", async ({ page }) => {
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const metrics = await page.evaluate(getSidebarMetrics());
    expect(metrics.hasCollapsed).toBe(true);
    expect(metrics.width).toBeLessThan(100);
  });

  test("Ctrl+B expand restores full width", async ({ page }) => {
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const metrics = await page.evaluate(getSidebarMetrics());
    expect(metrics.hasCollapsed).toBe(false);
    expect(metrics.width).toBeGreaterThan(200);
  });

  test("sidebar transition includes min-width property", async ({ page }) => {
    const metrics = await page.evaluate(getSidebarMetrics());
    expect(metrics.transition).toContain("min-width");
    expect(metrics.transition).toContain("width");
  });

  test("collapse hides labels and section titles", async ({ page }) => {
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const labelsHidden = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const labels = sidebar?.shadowRoot?.querySelectorAll(".label") ?? [];
      return [...labels].every(l => {
        const style = getComputedStyle(l);
        return style.display === "none" || l.offsetWidth === 0;
      });
    })()`);
    expect(labelsHidden).toBe(true);
  });

  test("expand shows labels and section titles", async ({ page }) => {
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const labelsVisible = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const labels = sidebar?.shadowRoot?.querySelectorAll(".nav-item .label") ?? [];
      return [...labels].some(l => l.offsetWidth > 0);
    })()`);
    expect(labelsVisible).toBe(true);
  });

  test("grid layout adjusts on collapse/expand", async ({ page }) => {
    const expandedGridColumns = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const layout = app?.shadowRoot?.querySelector(".layout");
      return getComputedStyle(layout).gridTemplateColumns;
    })()`);

    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const collapsedGridColumns = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const layout = app?.shadowRoot?.querySelector(".layout");
      return getComputedStyle(layout).gridTemplateColumns;
    })()`);

    expect(expandedGridColumns).not.toEqual(collapsedGridColumns);
  });

  test("sidebar persists collapsed state in localStorage", async ({ page }) => {
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const stored = await page.evaluate(() => localStorage.getItem("hu-sidebar-collapsed"));
    expect(stored).toBe("true");

    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const storedExpanded = await page.evaluate(() => localStorage.getItem("hu-sidebar-collapsed"));
    expect(storedExpanded).toBe("false");
  });

  test("collapse button chevron rotates on collapse", async ({ page }) => {
    const initialRotation = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const icon = sidebar?.shadowRoot?.querySelector(".collapse-btn .icon");
      return getComputedStyle(icon).transform;
    })()`);

    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const collapsedRotation = await page.evaluate(`(() => {
      const app = document.querySelector("hu-app");
      const sidebar = app?.shadowRoot?.querySelector("hu-sidebar");
      const icon = sidebar?.shadowRoot?.querySelector(".collapse-btn .icon");
      return getComputedStyle(icon).transform;
    })()`);

    expect(initialRotation).not.toEqual(collapsedRotation);
  });

  test("double-toggle returns to original state", async ({ page }) => {
    const before = await page.evaluate(getSidebarMetrics());

    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);
    await page.keyboard.press("Control+b");
    await page.waitForTimeout(400);

    const after = await page.evaluate(getSidebarMetrics());
    expect(after.hasCollapsed).toBe(before.hasCollapsed);
    expect(Math.abs(after.width - before.width)).toBeLessThan(2);
  });
});
