/**
 * Shared Shadow DOM traversal helpers for Playwright e2e tests.
 *
 * Each function returns a JS expression string for use with page.evaluate().
 * All helpers traverse: document → hu-app shadowRoot → <viewTag> shadowRoot.
 */

export function shadowExists(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return !!view?.shadowRoot?.querySelector("${selector}");
  })()`;
}

/** Check if selector exists inside a container's shadow root (for nested web components). */
export function shadowExistsIn(
  viewTag: string,
  containerSelector: string,
  innerSelector: string,
): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const container = view?.shadowRoot?.querySelector("${containerSelector}");
    return !!container?.shadowRoot?.querySelector("${innerSelector}");
  })()`;
}

/** Count elements inside a nested container's shadow root. */
export function shadowCountIn(
  viewTag: string,
  containerSelector: string,
  innerSelector: string,
): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const container = view?.shadowRoot?.querySelector("${containerSelector}");
    return container?.shadowRoot?.querySelectorAll("${innerSelector}").length ?? 0;
  })()`;
}

/** Get text content inside a nested container's shadow root. */
export function shadowTextIn(viewTag: string, containerSelector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const container = view?.shadowRoot?.querySelector("${containerSelector}");
    return container?.shadowRoot?.textContent ?? "";
  })()`;
}

export function shadowCount(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return view?.shadowRoot?.querySelectorAll("${selector}").length ?? 0;
  })()`;
}

export function shadowText(viewTag: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return view?.shadowRoot?.textContent ?? "";
  })()`;
}

/** Returns text content of a specific element within a view's shadow DOM. */
export function shadowElementText(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    return el?.textContent?.trim() ?? "";
  })()`;
}

/** Clicks an element within a view's shadow DOM. Returns a JS expression for page.evaluate. */
export function shadowClick(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    el?.click();
  })()`;
}

export function shadowComputedStyle(viewTag: string, selector: string, prop: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    if (!el) return "";
    return getComputedStyle(el).getPropertyValue("${prop}");
  })()`;
}

export function shadowBoundingRect(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { width: r.width, height: r.height, top: r.top, left: r.left };
  })()`;
}

/** Check DOM order: returns true if elA appears before elB in tree order. */
export function shadowDomOrder(viewTag: string, selectorA: string, selectorB: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const a = view?.shadowRoot?.querySelector("${selectorA}");
    const b = view?.shadowRoot?.querySelector("${selectorB}");
    if (!a || !b) return null;
    return !!(a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING);
  })()`;
}

/** Get all interactive element bounding rects within a view's shadow root. */
export function shadowInteractiveRects(viewTag: string): string {
  return `(() => {
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    if (!view?.shadowRoot) return [];
    const sels = 'button, a[href], input, select, textarea, [role="button"], [role="tab"], [role="link"]';
    const els = view.shadowRoot.querySelectorAll(sels);
    return [...els].map(el => {
      const r = el.getBoundingClientRect();
      const text = el.textContent?.trim() ?? "";
      const label = el.getAttribute("aria-label") ?? "";
      const title = el.getAttribute("title") ?? "";
      const tag = el.tagName.toLowerCase();
      const disabled = el.hasAttribute("disabled") || el.getAttribute("aria-disabled") === "true";
      return { width: r.width, height: r.height, text, label, title, tag, disabled };
    }).filter(e => e.width > 0 && e.height > 0);
  })()`;
}

/** View tags for each view name. */
export const VIEW_TAGS: Record<string, string> = {
  overview: "hu-overview-view",
  chat: "hu-chat-view",
  agents: "hu-agents-view",
  models: "hu-models-view",
  config: "hu-config-view",
  tools: "hu-tools-view",
  channels: "hu-channels-view",
  automations: "hu-automations-view",
  skills: "hu-skills-view",
  voice: "hu-voice-view",
  nodes: "hu-nodes-view",
  usage: "hu-usage-view",
  metrics: "hu-metrics-view",
  security: "hu-security-view",
  logs: "hu-logs-view",
};

export const ALL_VIEWS = Object.keys(VIEW_TAGS);

/** Initial wait after navigation for demo gateway to connect and data to load. */
export const WAIT = 1800;

/** Poll timeout for expect().toPass() — how long to retry assertions. */
export const POLL = 8000;

/**
 * Short wait for animations to settle (e.g. sheet open, theme switch).
 * Use sparingly — prefer deterministic waits (waitForSelector, expect().toPass).
 */
export const ANIMATION_SETTLE_MS = 200;

/**
 * Deep text extraction that traverses through ALL nested shadow DOMs.
 * Use when content is inside nested web components (tables, cards, etc.).
 */
export function deepText(viewTag: string): string {
  return `(() => {
    function collectText(root) {
      let text = "";
      if (!root) return text;
      const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
      while (walker.nextNode()) text += walker.currentNode.textContent;
      for (const el of root.querySelectorAll("*")) {
        if (el.shadowRoot) text += collectText(el.shadowRoot);
      }
      return text;
    }
    const app = document.querySelector("hu-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return view?.shadowRoot ? collectText(view.shadowRoot) : "";
  })()`;
}

/**
 * Waits for a view to be attached and have rendered content (LitElement).
 * Use after navigation instead of waitForTimeout.
 */
export async function waitForViewReady(
  page: import("@playwright/test").Page,
  viewTag: string,
  timeout = 5000,
): Promise<void> {
  await page.waitForLoadState("domcontentloaded");
  await page.waitForFunction(
    (tag) => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector(tag);
      return !!view && (view?.shadowRoot?.children.length ?? 0) > 0;
    },
    viewTag,
    { timeout },
  );
}

/**
 * Waits for a selector inside a view's shadow DOM.
 * Use for LitElement rendering (e.g. detail sheet, panel).
 */
export async function waitForShadowSelector(
  page: import("@playwright/test").Page,
  viewTag: string,
  selector: string,
  timeout = 5000,
): Promise<void> {
  await page.waitForFunction(
    ([tag, sel]) => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector(tag);
      return !!view?.shadowRoot?.querySelector(sel);
    },
    [viewTag, selector] as [string, string],
    { timeout },
  );
}
