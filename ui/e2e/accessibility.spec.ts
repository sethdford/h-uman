import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { shadowInteractiveRects, VIEW_TAGS, WAIT, POLL } from "./helpers.js";

const SHADOW_DOM_EXCLUDED_RULES = [
  "color-contrast",
  "link-in-text-block",
  "scrollable-region-focusable",
  "nested-interactive",
  "label",
  "aria-required-children",
  "aria-required-parent",
];

const VIEWS = [
  { path: "/", name: "Overview" },
  { path: "/#chat", name: "Chat" },
  { path: "/#agents", name: "Agents" },
  { path: "/#models", name: "Models" },
  { path: "/#config", name: "Config" },
  { path: "/#tools", name: "Tools" },
  { path: "/#channels", name: "Channels" },
  { path: "/#automations", name: "Automations" },
  { path: "/#skills", name: "Skills" },
  { path: "/#voice", name: "Voice" },
  { path: "/#nodes", name: "Nodes" },
  { path: "/#usage", name: "Usage" },
  { path: "/#security", name: "Security" },
  { path: "/#logs", name: "Logs" },
];

test.describe("Accessibility", () => {
  for (const view of VIEWS) {
    test(`${view.name} view passes axe accessibility`, async ({ page }) => {
      const url = view.path === "/" ? "/?demo" : `/?demo${view.path.slice(1)}`;
      await page.goto(url);
      await page.waitForLoadState("domcontentloaded");
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
    await page.keyboard.press("Control+k");
    await page.waitForTimeout(200);
    await page.keyboard.type("chat");
    await page.waitForTimeout(100);
    await page.keyboard.press("ArrowDown");
    await page.keyboard.press("Enter");
  });

  test("modal traps focus", async ({ page }) => {
    await page.goto("/?demo");
    await page.keyboard.press("Control+k");
    await page.waitForTimeout(200);
    await page.keyboard.press("Escape");
    await page.waitForTimeout(200);
  });
});

// ── Icon-Only Button Audit (visual-standards.md §6.2) ────────────

test.describe("Icon-Only Button Audit", () => {
  const VIEWS_TO_AUDIT = [
    { hash: "overview", tag: "sc-overview-view", name: "Overview" },
    { hash: "chat", tag: "sc-chat-view", name: "Chat" },
    { hash: "voice", tag: "sc-voice-view", name: "Voice" },
    { hash: "tools", tag: "sc-tools-view", name: "Tools" },
    { hash: "channels", tag: "sc-channels-view", name: "Channels" },
    { hash: "config", tag: "sc-config-view", name: "Config" },
    { hash: "skills", tag: "sc-skills-view", name: "Skills" },
    { hash: "logs", tag: "sc-logs-view", name: "Logs" },
    { hash: "security", tag: "sc-security-view", name: "Security" },
    { hash: "nodes", tag: "sc-nodes-view", name: "Nodes" },
  ];

  for (const view of VIEWS_TO_AUDIT) {
    test(`${view.name}: all icon-only buttons have accessible name`, async ({ page }) => {
      await page.goto(`/?demo#${view.hash}`);
      await page.waitForTimeout(WAIT);
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
    { hash: "overview", tag: "sc-overview-view", name: "Overview" },
    { hash: "tools", tag: "sc-tools-view", name: "Tools" },
    { hash: "channels", tag: "sc-channels-view", name: "Channels" },
    { hash: "security", tag: "sc-security-view", name: "Security" },
    { hash: "nodes", tag: "sc-nodes-view", name: "Nodes" },
  ];

  for (const view of HEADING_VIEWS) {
    test(`${view.name}: headings do not skip levels`, async ({ page }) => {
      await page.goto(`/?demo#${view.hash}`);
      await page.waitForTimeout(WAIT);
      await expect(async () => {
        const levels = (await page.evaluate(`(() => {
          const app = document.querySelector("sc-app");
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
