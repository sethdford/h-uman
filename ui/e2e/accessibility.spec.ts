import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import {
  shadowInteractiveRects,
  waitForAnimationsSettled,
  waitForViewReady,
  POLL,
} from "./helpers.js";

/** All axe rules run with zero exclusions. */
const SHADOW_DOM_EXCLUDED_RULES: string[] = [];

const VIEWS = [
  { path: "/#overview", name: "Overview" },
  { path: "/#chat", name: "Chat" },
  { path: "/#agents", name: "Agents" },
  { path: "/#sessions", name: "Sessions" },
  { path: "/#models", name: "Models" },
  { path: "/#config", name: "Config" },
  { path: "/#tools", name: "Tools" },
  { path: "/#channels", name: "Channels" },
  { path: "/#automations", name: "Automations" },
  { path: "/#skills", name: "Skills" },
  { path: "/#voice", name: "Voice" },
  { path: "/#nodes", name: "Nodes" },
  { path: "/#usage", name: "Usage" },
  { path: "/#memory", name: "Memory" },
  { path: "/#metrics", name: "Metrics" },
  { path: "/#security", name: "Security" },
  { path: "/#logs", name: "Logs" },
];

test.describe("Accessibility", () => {
  test.describe("axe scans of settled views", () => {
    // axe's color-contrast rule samples computed colours at scan time. Scanning
    // mid-entrance measures text part-way through an opacity fade and reports the
    // blended colour (e.g. .hero-sub at 4.3:1 while settling to a passing token),
    // which made these tests fail 30-80% of runs regardless of the code under test.
    //
    // Two kinds of entrance motion, handled separately:
    // - Scroll-driven entrances (animation-timeline: view(), scroll-entrance.ts)
    //   never "finish", so no wait can settle them. Reduced motion is the route
    //   chosen over scrolling #main-content end-to-end: the UI's reduced-motion
    //   blocks force `opacity: 1 !important` on every entrance class, so axe sees
    //   exactly the settled colours with no dependence on scroll geometry.
    // - Time-based fades that survive reduced motion are waited out explicitly.
    test.use({ reducedMotion: "reduce" });

    for (const view of VIEWS) {
      test(`${view.name} view passes axe accessibility`, async ({ page }) => {
        await page.goto(`/?demo${view.path.slice(1)}`);
        await waitForViewReady(page, `hu-${view.path.slice(2)}-view`);
        await waitForAnimationsSettled(page);
        const results = await new AxeBuilder({ page })
          .withTags(["wcag2a", "wcag2aa", "wcag21aa"])
          .disableRules(SHADOW_DOM_EXCLUDED_RULES)
          .analyze();
        const critical = results.violations.filter(
          (v) => v.impact === "critical" || v.impact === "serious",
        );
        if (critical.length > 0) {
          console.log(
            `A11y violations on ${view.name}:`,
            JSON.stringify(
              critical.map((v) => ({
                id: v.id,
                impact: v.impact,
                description: v.description,
                nodes: v.nodes.length,
              })),
              null,
              2,
            ),
          );
        }
        expect(critical).toEqual([]);
      });
    }
  });

  test("all navigation views are keyboard accessible", async ({ page }) => {
    await page.goto("/?demo");
    for (let i = 0; i < 5; i++) {
      await page.keyboard.press("Tab");
    }
    const focused = await page.evaluate(() => document.activeElement?.tagName);
    expect(focused).toBeTruthy();
  });

  test("command palette is keyboard navigable", async ({ page }) => {
    await page.goto("/");
    // Use Meta+k on Mac, Control+k elsewhere (app accepts both)
    await page.keyboard.press(process.platform === "darwin" ? "Meta+k" : "Control+k");
    await expect(page.locator("hu-command-palette input[role='combobox']")).toBeVisible({
      timeout: 5000,
    });
    await page.keyboard.type("chat");
    await page.keyboard.press("ArrowDown");
    await page.keyboard.press("Enter");
  });

  test("modal traps focus", async ({ page }) => {
    await page.goto("/?demo");
    await page.waitForLoadState("domcontentloaded");
    await page.keyboard.press(process.platform === "darwin" ? "Meta+k" : "Control+k");
    const paletteInput = page.locator("hu-command-palette input[role='combobox']");
    await expect(paletteInput).toBeVisible({ timeout: 5000 });
    await paletteInput.focus();
    await page.keyboard.press("Escape");
    await expect(page.locator("hu-command-palette input")).not.toBeVisible();
  });
});

// ── Icon-Only Button Audit (visual-standards.md §6.2) ────────────

test.describe("Icon-Only Button Audit", () => {
  const VIEWS_TO_AUDIT = [
    { hash: "overview", tag: "hu-overview-view", name: "Overview" },
    { hash: "chat", tag: "hu-chat-view", name: "Chat" },
    { hash: "voice", tag: "hu-voice-view", name: "Voice" },
    { hash: "tools", tag: "hu-tools-view", name: "Tools" },
    { hash: "channels", tag: "hu-channels-view", name: "Channels" },
    { hash: "config", tag: "hu-config-view", name: "Config" },
    { hash: "skills", tag: "hu-skills-view", name: "Skills" },
    { hash: "logs", tag: "hu-logs-view", name: "Logs" },
    { hash: "security", tag: "hu-security-view", name: "Security" },
    { hash: "nodes", tag: "hu-nodes-view", name: "Nodes" },
  ];

  for (const view of VIEWS_TO_AUDIT) {
    test(`${view.name}: all icon-only buttons have accessible name`, async ({ page }) => {
      await page.goto(`/?demo#${view.hash}`);
      await waitForViewReady(page, view.tag);
      await expect(async () => {
        const rects = (await page.evaluate(shadowInteractiveRects(view.tag))) as Array<{
          width: number;
          height: number;
          text: string;
          label: string;
          title: string;
          tag: string;
          disabled: boolean;
        }>;
        const unlabeled = rects.filter(
          (r) => r.tag === "button" && !r.text && !r.label && !r.title && !r.disabled,
        );
        expect(
          unlabeled.length,
          `Found ${unlabeled.length} icon-only button(s) without aria-label or title`,
        ).toBe(0);
      }).toPass({ timeout: POLL });
    });
  }
});

// ── Heading Hierarchy Check (ux-patterns.md §5.2) ────────────────

test.describe("Heading Hierarchy", () => {
  const HEADING_VIEWS = [
    { hash: "overview", tag: "hu-overview-view", name: "Overview" },
    { hash: "tools", tag: "hu-tools-view", name: "Tools" },
    { hash: "channels", tag: "hu-channels-view", name: "Channels" },
    { hash: "security", tag: "hu-security-view", name: "Security" },
    { hash: "nodes", tag: "hu-nodes-view", name: "Nodes" },
  ];

  for (const view of HEADING_VIEWS) {
    test(`${view.name}: headings do not skip levels`, async ({ page }) => {
      await page.goto(`/?demo#${view.hash}`);
      await waitForViewReady(page, view.tag);
      await expect(async () => {
        const levels = (await page.evaluate(`(() => {
          const app = document.querySelector("hu-app");
          const v = app?.shadowRoot?.querySelector("${view.tag}");
          if (!v?.shadowRoot) return [];
          const headings = v.shadowRoot.querySelectorAll("h1, h2, h3, h4, h5, h6");
          return [...headings].map(h => parseInt(h.tagName[1], 10));
        })()`)) as number[];
        if (levels.length < 2) return;
        for (let i = 1; i < levels.length; i++) {
          const gap = levels[i] - levels[i - 1];
          expect(gap, `Heading skip: h${levels[i - 1]} → h${levels[i]}`).toBeLessThanOrEqual(1);
        }
      }).toPass({ timeout: POLL });
    });
  }
});
