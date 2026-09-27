import { test, expect, type Page } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { shadowInteractiveRects, waitForViewReady, POLL } from "./helpers.js";

/** All axe rules run with zero exclusions. */
const SHADOW_DOM_EXCLUDED_RULES: string[] = [];

/**
 * Waits until the view shows its loaded data at its resting frame, so axe scans
 * what a user sees rather than a loading skeleton or a half-faded entrance.
 * Contrast checked mid-fade (hero, composer, .view-enter) blends foreground into
 * background and fails at random ratios.
 *
 * - Busy while the demo gateway is still connecting (fixed 400 ms) or has any
 *   request inside its random 80-280 ms latency: a time-only quiet window can
 *   close in that gap and scan the page before its data renders.
 * - Time-based animations: wait until none has run for `quietMs`, looping
 *   because every data arrival starts a new entrance wave.
 * - Scroll-driven animations (`animation-timeline: view()` reveals such as
 *   hu-card-enter): cancelled inside the quiet window, so any transition the
 *   cancel itself starts is waited on too. Nobody scrolls a headless page, so
 *   below-the-fold cards would otherwise sit at partial opacity forever.
 * - Ignored: anything not `running` (idle transitions never settle `finished`),
 *   infinite loops (status pulses), and transitions of properties axe never
 *   reads for contrast: `filter` (the 3s ambient-warmth sepia on hu-app) and
 *   `scrollbar-color` (re-created on every style flush for the Skills view's
 *   `transition: all` tag chips, so it would never go quiet).
 *
 * Walks shadow roots because Chromium's document.getAnimations() omits
 * animations inside them.
 */
async function settlePage(page: Page, quietMs = 250, timeout = 5000): Promise<void> {
  await page.evaluate(
    async ({ quietMs, timeout }) => {
      const all = (): Animation[] => {
        const out = new Set(document.getAnimations());
        const walk = (root: Document | ShadowRoot) => {
          for (const el of root.querySelectorAll("*")) {
            for (const a of el.getAnimations()) out.add(a);
            if (el.shadowRoot) walk(el.shadowRoot);
          }
        };
        walk(document);
        return [...out];
      };
      const isPending = (a: Animation): boolean =>
        a.playState === "running" &&
        a.timeline instanceof DocumentTimeline &&
        a.effect?.getComputedTiming().iterations !== Infinity &&
        !["filter", "scrollbar-color"].includes((a as CSSTransition).transitionProperty);
      const gatewayBusy = (): boolean => {
        const gw = (document.querySelector("hu-app") as { gateway?: unknown } | null)?.gateway as
          { status?: string; inFlight?: number } | undefined;
        return gw?.status !== "connected" || (gw.inFlight ?? 0) > 0;
      };
      const deadline = performance.now() + timeout;
      let quietSince = performance.now();
      while (performance.now() - quietSince < quietMs) {
        const anims = all();
        const scrollDriven = anims.filter((a) => !(a.timeline instanceof DocumentTimeline));
        for (const a of scrollDriven) a.cancel();
        const busy = gatewayBusy();
        const active = anims.filter(isPending);
        if (performance.now() > deadline) {
          const names = active.map(
            (a) => (a as CSSAnimation).animationName ?? (a as CSSTransition).transitionProperty,
          );
          const gw = busy ? "gateway busy; " : "";
          throw new Error(`page not settled after ${timeout}ms: ${gw}${names.join(", ")}`);
        }
        if (busy || scrollDriven.length > 0) {
          quietSince = performance.now();
        } else if (active.length > 0) {
          await Promise.race([
            Promise.all(active.map((a) => a.finished.catch(() => {}))),
            new Promise((r) => setTimeout(r, deadline - performance.now())),
          ]);
          quietSince = performance.now();
        }
        await new Promise((r) => setTimeout(r, 50));
      }
    },
    { quietMs, timeout },
  );
}

/**
 * Serious/critical violations present in the settled UI when this check started
 * waiting for data and animations (2026-09-27); before that it scanned loading skeletons
 * and never reached them. Each is a real bug with its own follow-up. Ratchet in
 * both directions: a view may not gain a rule or nodes, and a fix must lower its
 * entry here so the freed slack cannot hide the next regression.
 */
const KNOWN_VIOLATIONS: Record<string, Record<string, number>> = {};

const VIEWS = [
  // An empty hash routes to chat, so Overview needs its own hash to be scanned at all.
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
  for (const view of VIEWS) {
    test(`${view.name} view passes axe accessibility`, async ({ page }) => {
      await page.goto(`/?demo${view.path.slice(1)}`);
      await page.waitForLoadState("domcontentloaded");
      await settlePage(page);
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
      const nodesByRule = Object.fromEntries(critical.map((v) => [v.id, v.nodes.length]));
      expect(nodesByRule, "update KNOWN_VIOLATIONS only to lower an entry").toEqual(
        KNOWN_VIOLATIONS[view.name] ?? {},
      );
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

  test("chat session list: Tab reaches it, arrows move, Enter selects, Delete removes", async ({
    page,
  }) => {
    await page.goto("/?demo#chat");
    await waitForViewReady(page, "hu-chat-view");
    const panel = page.locator("hu-chat-sessions-panel");
    const rows = panel.locator(".session-item");
    await expect(rows.first()).toBeVisible({ timeout: POLL });
    const before = await rows.count();
    expect(before).toBeGreaterThan(2);
    // Id of the row holding focus, read through both shadow roots.
    const focusedRow = () =>
      page.evaluate(() => {
        let a: Element | null = document.activeElement;
        while (a?.shadowRoot?.activeElement) a = a.shadowRoot.activeElement;
        return (a as HTMLElement | null)?.closest<HTMLElement>(".session-item")?.dataset.sessionId;
      });
    const ids = await rows.evaluateAll((els) =>
      els.map((e) => (e as HTMLElement).dataset.sessionId),
    );

    // The list is one Tab stop: search box, then a single session row.
    await panel.locator(".search-input, input[aria-label='Search sessions']").first().focus();
    await page.keyboard.press("Tab");
    const first = await focusedRow();
    expect(first).toBeTruthy();
    const start = ids.indexOf(first);
    // Tab again reaches that row's Delete, and once more leaves the list.
    await page.keyboard.press("Tab");
    expect(await focusedRow()).toBe(first);
    await page.keyboard.press("Tab");
    expect(await focusedRow()).toBeUndefined();
    await panel.locator(".session-item .session-open[tabindex='0']").focus();

    await page.keyboard.press("ArrowDown");
    expect(await focusedRow()).toBe(ids[start + 1]);

    // Enter selects: the chat switches to that session, which the row announces.
    await page.keyboard.press("Enter");
    await expect(panel.locator(".session-open[aria-current='true']")).toHaveCount(1);
    await expect(rows.nth(start + 1).locator(".session-open")).toHaveAttribute(
      "aria-current",
      "true",
    );

    await rows
      .nth(start + 1)
      .locator(".session-open")
      .focus();
    await page.keyboard.press("Delete");
    await expect(rows).toHaveCount(before - 1);
    await expect(panel.locator(`.session-item[data-session-id="${ids[start + 1]}"]`)).toHaveCount(
      0,
    );
    expect(await focusedRow()).toBe(ids[start + 2]);
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
