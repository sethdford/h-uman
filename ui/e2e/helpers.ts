/**
 * Shared Shadow DOM traversal helpers for Playwright e2e tests.
 *
 * Each function returns a JS expression string for use with page.evaluate().
 * All helpers traverse: document → sc-app shadowRoot → <viewTag> shadowRoot.
 */

export function shadowExists(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return !!view?.shadowRoot?.querySelector("${selector}");
  })()`;
}

export function shadowCount(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return view?.shadowRoot?.querySelectorAll("${selector}").length ?? 0;
  })()`;
}

export function shadowText(viewTag: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    return view?.shadowRoot?.textContent ?? "";
  })()`;
}

/** Returns text content of a specific element within a view's shadow DOM. */
export function shadowElementText(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    return el?.textContent?.trim() ?? "";
  })()`;
}

/** Clicks an element within a view's shadow DOM. Returns a JS expression for page.evaluate. */
export function shadowClick(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    el?.click();
  })()`;
}

export function shadowComputedStyle(viewTag: string, selector: string, prop: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
    const view = app?.shadowRoot?.querySelector("${viewTag}");
    const el = view?.shadowRoot?.querySelector("${selector}");
    if (!el) return "";
    return getComputedStyle(el).getPropertyValue("${prop}");
  })()`;
}

export function shadowBoundingRect(viewTag: string, selector: string): string {
  return `(() => {
    const app = document.querySelector("sc-app");
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
    const app = document.querySelector("sc-app");
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
    const app = document.querySelector("sc-app");
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
  overview: "sc-overview-view",
  chat: "sc-chat-view",
  agents: "sc-agents-view",
  models: "sc-models-view",
  config: "sc-config-view",
  tools: "sc-tools-view",
  channels: "sc-channels-view",
  automations: "sc-automations-view",
  skills: "sc-skills-view",
  voice: "sc-voice-view",
  nodes: "sc-nodes-view",
  usage: "sc-usage-view",
  security: "sc-security-view",
  logs: "sc-logs-view",
};

export const ALL_VIEWS = Object.keys(VIEW_TAGS);

export const WAIT = 1800;
export const POLL = 8000;
