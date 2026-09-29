import { describe, it, expect, vi } from "vitest";

/* hu-error-boundary renders one app-wide `_viewError` for the CURRENT tab, so a
 * background prefetch of some other tab must never write it. These views stand
 * in for real chunks: two load, one fails like a chunk that 404s after a deploy. */
vi.mock("./views/design-system-view.js", () => ({}));
vi.mock("./views/settings-view.js", () => ({}));
vi.mock("./views/workflow-view.js", () => {
  throw new Error("workflow chunk failed to load");
});

await import("./app.js");

type TabId = "overview" | "design-system" | "settings" | "workflow";

/* The element is created but never connected, so connectedCallback's gateway and
 * hash-routing side effects stay out of the test; only the view-loading state
 * machine runs. */
interface AppInternals extends HTMLElement {
  tab: TabId;
  _viewError: Error | null;
  _ensureLoaded(tab: TabId): Promise<unknown>;
  _prefetchSilent(tab: TabId): void;
  _applyHashRoute(tab: TabId): Promise<void>;
}

function makeApp(): AppInternals {
  const app = document.createElement("hu-app") as AppInternals;
  /* A disconnected LitElement's updateComplete never settles, and _switchView
   * awaits it. happy-dom has no startViewTransition, so the switch itself is
   * the plain _performViewSwitch assignment under test. */
  Object.defineProperty(app, "updateComplete", { value: Promise.resolve(true) });
  return app;
}

describe("hu-app _viewError scoping", () => {
  it("keeps the current tab's error when an unrelated prefetch succeeds", async () => {
    const app = makeApp();
    app.tab = "overview";
    const err = new Error("overview failed to load");
    app._viewError = err;

    await app._ensureLoaded("design-system");

    expect(app.tab).toBe("overview");
    expect(app._viewError).toBe(err);
  });

  it("clears the previous tab's error when navigating to a tab that loads", async () => {
    const app = makeApp();
    app.tab = "overview";
    app._viewError = new Error("overview failed to load");

    await app._applyHashRoute("settings");

    expect(app.tab).toBe("settings");
    expect(app._viewError).toBeNull();
  });

  it("does not record a failed prefetch of a tab the user is not on", async () => {
    const app = makeApp();
    app.tab = "overview";
    expect(app._viewError).toBeNull();

    await app._ensureLoaded("workflow");

    expect(app.tab).toBe("overview");
    expect(app._viewError).toBeNull();
  });

  it("records the error when navigating to a tab whose chunk fails", async () => {
    const app = makeApp();
    app.tab = "overview";

    await app._applyHashRoute("workflow");

    expect(app.tab).toBe("workflow");
    /* vitest wraps a throwing mock factory's error; the original is its cause. */
    const err = app._viewError as (Error & { cause?: Error }) | null;
    expect(err).not.toBeNull();
    expect((err?.cause ?? err)?.message).toBe("workflow chunk failed to load");
  });
});
