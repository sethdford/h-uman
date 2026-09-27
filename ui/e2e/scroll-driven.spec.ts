import { test, expect } from "@playwright/test";
import { waitForViewReady } from "./helpers.js";

/**
 * Regression test for the minifier folding `animation-timeline` into the `animation`
 * shorthand. Lightning CSS (Vite's CSS minifier) rewrote
 *   animation: hu-scroll-entrance linear both; animation-timeline: view();
 * into
 *   animation: linear both hu-scroll-entrance view();
 * Chromium rejects a timeline inside the shorthand and drops the whole declaration, so
 * every global scroll-driven utility in theme.css / scroll-driven.css was dead in the
 * built bundle while the source looked correct.
 *
 * The global utilities only match light-DOM elements (document styles never reach a
 * shadow root), so the probes are appended to the document of a real dashboard view
 * served from the BUILT bundle (vite preview). Injecting styles with addStyleTag would
 * bypass the minifier and could not catch this.
 */
const GLOBAL_SCROLL_UTILITIES: { name: string; className: string; child: boolean }[] = [
  { name: "hu-scroll-entrance", className: "hu-scroll-reveal", child: false },
  { name: "hu-scroll-entrance", className: "hu-scroll-reveal-stagger", child: true },
  { name: "hu-scroll-grow", className: "hu-scroll-progress", child: false },
  { name: "hu-parallax-shift", className: "hu-parallax-subtle", child: false },
  { name: "hu-chapter-reveal", className: "hu-chapter-reveal", child: false },
  { name: "hu-data-point-enter", className: "hu-data-cascade", child: true },
  { name: "hu-stroke-draw", className: "hu-connection-draw", child: false },
  { name: "hu-parallax-deep-shift", className: "hu-parallax-deep", child: false },
  { name: "hu-glass-scroll-deepen", className: "hu-glass-scroll-aware", child: false },
];

test("global scroll-driven utilities run on a scroll timeline in the built bundle", async ({
  page,
}) => {
  await page.goto("/?demo#metrics");
  await waitForViewReady(page, "hu-metrics-view");
  // The probes cover both view() and scroll() utilities, so both must be supported.
  const supported = await page.evaluate(
    () =>
      CSS.supports("animation-timeline: view()") && CSS.supports("animation-timeline: scroll()"),
  );
  test.skip(!supported, "requires animation-timeline: view() and scroll() support");

  const timelines = await page.evaluate((utilities) => {
    // A scrollable light-DOM container so both view() and scroll(nearest) timelines
    // resolve to an active scroller.
    const scroller = document.createElement("div");
    scroller.style.cssText = "position:fixed;inset:0;overflow:auto;z-index:-1";
    document.body.append(scroller);
    const targets = utilities.map((u) => {
      const host = document.createElement("div");
      host.className = u.className;
      host.style.height = "40px";
      const target = u.child ? host.appendChild(document.createElement("div")) : host;
      target.style.height = "40px";
      scroller.append(host);
      return target;
    });
    const spacer = document.createElement("div");
    spacer.style.height = "300vh";
    scroller.append(spacer);

    const result: Record<string, string[]> = {};
    utilities.forEach((u, i) => {
      result[u.className] = targets[i]
        .getAnimations()
        .filter((a) => (a as CSSAnimation).animationName === u.name)
        .map((a) => a.timeline?.constructor.name ?? "null");
    });
    return result;
  }, GLOBAL_SCROLL_UTILITIES);

  for (const u of GLOBAL_SCROLL_UTILITIES) {
    const kinds = timelines[u.className];
    expect(kinds, `.${u.className} has no ${u.name} animation`).not.toHaveLength(0);
    for (const kind of kinds) {
      expect(["ViewTimeline", "ScrollTimeline"], `.${u.className} runs on ${kind}`).toContain(kind);
    }
  }
});

/**
 * Component-scoped entrance animations (Lit `scrollEntranceStyles`, which lives inside
 * each shadow root). Two classes of element carried the entrance class but never
 * animated: the metrics view's `.hu-scroll-reveal` sections (the shared styles only
 * defined the `-stagger` variant) and the skill registry's cards (the registry never
 * adopted the shared styles at all).
 */
test.describe("component scroll entrances run on a view timeline", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?demo#metrics");
    const supported = await page.evaluate(() => CSS.supports("animation-timeline: view()"));
    test.skip(!supported, "requires animation-timeline: view() support");
  });

  test("metrics .hu-scroll-reveal sections", async ({ page }) => {
    await waitForViewReady(page, "hu-metrics-view");
    const sections = await page.evaluate(() => {
      const view = document.querySelector("hu-app")?.shadowRoot?.querySelector("hu-metrics-view");
      return [...(view?.shadowRoot?.querySelectorAll(".hu-scroll-reveal") ?? [])].map((el) =>
        el.getAnimations().map((a) => a.timeline?.constructor.name ?? "null"),
      );
    });
    expect(sections.length, "metrics renders .hu-scroll-reveal sections").toBeGreaterThan(0);
    for (const kinds of sections) expect(kinds).toContain("ViewTimeline");
  });

  test("skill registry cards", async ({ page }) => {
    await page.goto("/?demo#skills");
    await waitForViewReady(page, "hu-skills-view");
    const readCards = () =>
      page.evaluate(() => {
        const registry = document
          .querySelector("hu-app")
          ?.shadowRoot?.querySelector("hu-skills-view")
          ?.shadowRoot?.querySelector("hu-skill-registry");
        return [
          ...(registry?.shadowRoot?.querySelectorAll(".hu-scroll-reveal-stagger > *") ?? []),
        ].map((el) => el.getAnimations().map((a) => a.timeline?.constructor.name ?? "null"));
      });
    await expect.poll(async () => (await readCards()).length).toBeGreaterThan(0);
    for (const kinds of await readCards()) expect(kinds).toContain("ViewTimeline");
  });
});
