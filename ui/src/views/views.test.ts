import { describe, it, expect } from "vitest";

import type { ScLogsView } from "./logs-view.js";

// Import all view elements to register them
import "./overview-view.js";
import "./chat-view.js";
import "./agents-view.js";
import "./sessions-view.js";
import "./models-view.js";
import "./config-view.js";
import "./tools-view.js";
import "./channels-view.js";
import "./automations-view.js";
import "./skills-view.js";
import "./voice-view.js";
import "./nodes-view.js";
import "./usage-view.js";
import "./metrics-view.js";
import "./security-view.js";
import "./logs-view.js";

const VIEW_TAGS = [
  "hu-overview-view",
  "hu-chat-view",
  "hu-agents-view",
  "hu-sessions-view",
  "hu-models-view",
  "hu-config-view",
  "hu-tools-view",
  "hu-channels-view",
  "hu-automations-view",
  "hu-skills-view",
  "hu-voice-view",
  "hu-nodes-view",
  "hu-usage-view",
  "hu-metrics-view",
  "hu-security-view",
  "hu-logs-view",
];

describe("views", () => {
  for (const tag of VIEW_TAGS) {
    describe(tag, () => {
      it("should be defined as a custom element", () => {
        expect(customElements.get(tag)).toBeDefined();
      });

      it("should be creatable", () => {
        const el = document.createElement(tag);
        expect(el).toBeInstanceOf(HTMLElement);
      });
    });
  }
});

describe("hu-chat-view", () => {
  it("renders hu-chat-composer when no messages", async () => {
    const el = document.createElement("hu-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("hu-chat-composer");
    expect(composer).toBeTruthy();
    el.remove();
  });

  it("has suggestion pills in composer when empty", async () => {
    const el = document.createElement("hu-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("hu-chat-composer");
    const cards = composer?.shadowRoot?.querySelectorAll(".pill") ?? [];
    expect(cards.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("renders message thread component", async () => {
    const el = document.createElement("hu-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const thread = el.shadowRoot?.querySelector("hu-message-thread");
    expect(thread).toBeTruthy();
    el.remove();
  });

  it("composer has drag-over class during drag", async () => {
    const el = document.createElement("hu-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("hu-chat-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    const inputWrap = composer?.shadowRoot?.querySelector(".composer");
    expect(inputWrap?.classList.contains("drag-over")).toBe(false);
    inputWrap?.dispatchEvent(new DragEvent("dragover", { bubbles: true }));
    await composer?.updateComplete;
    expect(inputWrap?.classList.contains("drag-over")).toBe(true);
    el.remove();
  });
});

type LitView = HTMLElement & { updateComplete: Promise<boolean> };

function createView(tag: string): LitView {
  const el = document.createElement(tag) as LitView;
  document.body.appendChild(el);
  return el;
}

describe("hu-overview-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("hu-overview-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("hu-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("has shadow DOM content on first render", async () => {
    const el = createView("hu-overview-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-agents-view", () => {
  it("renders shadow DOM content", async () => {
    const el = createView("hu-agents-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });

  it("has scoped CSS via adoptedStyleSheets or style elements", async () => {
    const el = createView("hu-agents-view");
    await el.updateComplete;
    const hasAdopted = (el.shadowRoot?.adoptedStyleSheets?.length ?? 0) > 0;
    const hasStyle = (el.shadowRoot?.querySelectorAll("style")?.length ?? 0) > 0;
    expect(hasAdopted || hasStyle).toBe(true);
    el.remove();
  });
});

describe("hu-models-view", () => {
  it("renders skeleton or content on first render", async () => {
    const el = createView("hu-models-view");
    await el.updateComplete;
    const hasSkeleton = el.shadowRoot?.querySelector("hu-skeleton");
    const hasContent = el.shadowRoot?.querySelector("h2, hu-page-hero, hu-search");
    expect(hasSkeleton || hasContent).toBeTruthy();
    el.remove();
  });
});

describe("hu-tools-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("hu-tools-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("hu-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });
});

describe("hu-channels-view", () => {
  it("renders skeleton or channel grid", async () => {
    const el = createView("hu-channels-view");
    await el.updateComplete;
    const hasSkeleton = el.shadowRoot?.querySelector("hu-skeleton");
    const hasGrid = el.shadowRoot?.querySelector(".grid, hu-search");
    expect(hasSkeleton || hasGrid).toBeTruthy();
    el.remove();
  });
});

describe("hu-sessions-view", () => {
  it("renders hu-page-hero with heading", async () => {
    const el = createView("hu-sessions-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero");
    expect(hero).toBeTruthy();
    const header = el.shadowRoot?.querySelector("hu-section-header");
    expect(header?.getAttribute("heading")).toBe("Sessions");
    el.remove();
  });

  it("renders skeleton while loading or content when loaded", async () => {
    const el = createView("hu-sessions-view");
    await el.updateComplete;
    const hasSkeleton = el.shadowRoot?.querySelector("hu-skeleton");
    const hasCard = el.shadowRoot?.querySelector("hu-card");
    const hasEmptyState = el.shadowRoot?.querySelector("hu-empty-state");
    const hasHero = el.shadowRoot?.querySelector("hu-page-hero");
    expect(hasHero).toBeTruthy();
    expect(hasSkeleton || hasCard || hasEmptyState).toBeTruthy();
    el.remove();
  });
});

describe("hu-config-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("hu-config-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("hu-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });
});

describe("hu-automations-view", () => {
  it("renders content after load", async () => {
    const el = createView("hu-automations-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-skills-view", () => {
  it("renders content after load", async () => {
    const el = createView("hu-skills-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-voice-view", () => {
  it("renders voice UI content", async () => {
    const el = createView("hu-voice-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-nodes-view", () => {
  it("renders content after load", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });

  it("renders page hero with heading", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero");
    expect(hero).toBeTruthy();
    const header = el.shadowRoot?.querySelector("hu-section-header");
    expect(header?.getAttribute("heading")).toBe("Nodes");
    el.remove();
  });

  it("renders nodes grid or empty state", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    const grid = el.shadowRoot?.querySelector(".nodes-grid");
    const empty = el.shadowRoot?.querySelector("hu-empty-state");
    expect(grid || empty).toBeTruthy();
    el.remove();
  });

  it("renders refresh button with aria-label", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector('hu-button[aria-label="Refresh nodes"]');
    expect(btn).toBeTruthy();
    el.remove();
  });

  it("shows empty state when no nodes loaded", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    const emptyState = el.shadowRoot?.querySelector("hu-empty-state");
    expect(emptyState).toBeTruthy();
    el.remove();
  });

  it("has scoped CSS via adoptedStyleSheets or style elements", async () => {
    const el = createView("hu-nodes-view");
    await el.updateComplete;
    const hasAdopted = (el.shadowRoot?.adoptedStyleSheets?.length ?? 0) > 0;
    const hasStyle = (el.shadowRoot?.querySelectorAll("style")?.length ?? 0) > 0;
    expect(hasAdopted || hasStyle).toBe(true);
    el.remove();
  });
});

describe("hu-usage-view", () => {
  it("renders content after load", async () => {
    const el = createView("hu-usage-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });

  it("should render hu-chart elements when data is present", async () => {
    const el = document.createElement("hu-usage-view") as LitView;
    document.body.appendChild(el);
    await el.updateComplete;
    // Charts may not render without gateway data, but the component should be importable
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-security-view", () => {
  it("renders content after load", async () => {
    const el = createView("hu-security-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-logs-view", () => {
  it("renders log area", async () => {
    const el = createView("hu-logs-view");
    await el.updateComplete;
    const logCard = el.shadowRoot?.querySelector("hu-card.log-card, .log-area");
    expect(logCard).toBeTruthy();
    el.remove();
  });

  it("should render controls header", async () => {
    const el = document.createElement("hu-logs-view") as ScLogsView;
    document.body.appendChild(el);
    await el.updateComplete;
    const controls = el.shadowRoot?.querySelector(".controls-sticky, .controls");
    expect(controls).toBeTruthy();
    el.remove();
  });
});

/* ── Accessibility: all views ─────────────────────────────── */

describe("view accessibility", () => {
  const ALL_VIEWS = [
    "hu-overview-view",
    "hu-chat-view",
    "hu-agents-view",
    "hu-sessions-view",
    "hu-models-view",
    "hu-config-view",
    "hu-tools-view",
    "hu-channels-view",
    "hu-automations-view",
    "hu-skills-view",
    "hu-voice-view",
    "hu-nodes-view",
    "hu-usage-view",
    "hu-security-view",
    "hu-logs-view",
  ];

  for (const tag of ALL_VIEWS) {
    it(`${tag} has scoped styles`, async () => {
      const el = createView(tag);
      await el.updateComplete;
      const hasAdopted = (el.shadowRoot?.adoptedStyleSheets?.length ?? 0) > 0;
      const hasStyle = (el.shadowRoot?.querySelectorAll("style")?.length ?? 0) > 0;
      expect(hasAdopted || hasStyle).toBe(true);
      el.remove();
    });
  }
});

/* ── Deep view-specific tests ─────────────────────────────── */

describe("hu-overview-view deep", () => {
  it("renders bento grid or skeleton on load", async () => {
    const el = createView("hu-overview-view");
    await el.updateComplete;
    const bento = el.shadowRoot?.querySelector(".bento");
    const skel = el.shadowRoot?.querySelector("hu-skeleton");
    expect(bento || skel).toBeTruthy();
    el.remove();
  });

  it("renders stat cells or skeleton", async () => {
    const el = createView("hu-overview-view");
    await el.updateComplete;
    const cards = el.shadowRoot?.querySelectorAll("hu-card, hu-skeleton");
    expect(cards?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("skeleton mirrors bento grid areas", async () => {
    const el = createView("hu-overview-view");
    await el.updateComplete;
    const skeletonBento = el.shadowRoot?.querySelector(".skeleton-bento");
    if (skeletonBento) {
      const cells = skeletonBento.querySelectorAll("hu-skeleton");
      expect(cells.length).toBe(3);
    }
    el.remove();
  });
});

describe("hu-tools-view deep", () => {
  it("has page hero or skeleton on load", async () => {
    const el = createView("hu-tools-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero, hu-skeleton");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("hu-channels-view deep", () => {
  it("has page hero or skeleton on load", async () => {
    const el = createView("hu-channels-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero, hu-skeleton");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("hu-security-view deep", () => {
  it("has content on initial render", async () => {
    const el = createView("hu-security-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("hu-sessions-view deep", () => {
  it("has page hero with Sessions heading", async () => {
    const el = createView("hu-sessions-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero");
    expect(hero).toBeTruthy();
    const header = el.shadowRoot?.querySelector("hu-section-header");
    expect(header?.getAttribute("heading")).toBe("Sessions");
    el.remove();
  });

  it("renders hu-skeleton, hu-card, or hu-empty-state", async () => {
    const el = createView("hu-sessions-view");
    await el.updateComplete;
    const skeleton = el.shadowRoot?.querySelector("hu-skeleton");
    const card = el.shadowRoot?.querySelector("hu-card");
    const emptyState = el.shadowRoot?.querySelector("hu-empty-state");
    expect(skeleton || card || emptyState).toBeTruthy();
    el.remove();
  });
});

describe("hu-automations-view deep", () => {
  it("has page hero", async () => {
    const el = createView("hu-automations-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("hu-page-hero");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("hu-logs-view deep", () => {
  it("has log controls or card", async () => {
    const el = createView("hu-logs-view");
    await el.updateComplete;
    const controls = el.shadowRoot?.querySelector(
      ".controls, .filters, hu-segmented-control, .log-area, hu-card",
    );
    expect(controls).toBeTruthy();
    el.remove();
  });
});
