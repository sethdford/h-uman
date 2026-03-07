import { describe, it, expect } from "vitest";

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
import "./security-view.js";
import "./logs-view.js";

const VIEW_TAGS = [
  "sc-overview-view",
  "sc-chat-view",
  "sc-agents-view",
  "sc-sessions-view",
  "sc-models-view",
  "sc-config-view",
  "sc-tools-view",
  "sc-channels-view",
  "sc-automations-view",
  "sc-skills-view",
  "sc-voice-view",
  "sc-nodes-view",
  "sc-usage-view",
  "sc-security-view",
  "sc-logs-view",
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

describe("sc-chat-view", () => {
  it("renders sc-composer when no messages", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("sc-composer");
    expect(composer).toBeTruthy();
    el.remove();
  });

  it("has suggested bento cards in composer when empty", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("sc-composer");
    const cards = composer?.shadowRoot?.querySelectorAll(".bento-card") ?? [];
    expect(cards.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("renders message list component", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const messageList = el.shadowRoot?.querySelector("sc-message-list");
    expect(messageList).toBeTruthy();
    el.remove();
  });

  it("composer has drag-over class during drag", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("sc-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    const inputWrap = composer?.shadowRoot?.querySelector(".input-wrap");
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

describe("sc-overview-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("sc-overview-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("sc-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("has shadow DOM content on first render", async () => {
    const el = createView("sc-overview-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-agents-view", () => {
  it("renders shadow DOM content", async () => {
    const el = createView("sc-agents-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });

  it("has scoped CSS via adoptedStyleSheets or style elements", async () => {
    const el = createView("sc-agents-view");
    await el.updateComplete;
    const hasAdopted = (el.shadowRoot?.adoptedStyleSheets?.length ?? 0) > 0;
    const hasStyle = (el.shadowRoot?.querySelectorAll("style")?.length ?? 0) > 0;
    expect(hasAdopted || hasStyle).toBe(true);
    el.remove();
  });
});

describe("sc-models-view", () => {
  it("renders skeleton or content on first render", async () => {
    const el = createView("sc-models-view");
    await el.updateComplete;
    const hasSkeleton = el.shadowRoot?.querySelector("sc-skeleton");
    const hasContent = el.shadowRoot?.querySelector("h2, sc-page-hero, sc-search");
    expect(hasSkeleton || hasContent).toBeTruthy();
    el.remove();
  });
});

describe("sc-tools-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("sc-tools-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("sc-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });
});

describe("sc-channels-view", () => {
  it("renders skeleton or channel grid", async () => {
    const el = createView("sc-channels-view");
    await el.updateComplete;
    const hasSkeleton = el.shadowRoot?.querySelector("sc-skeleton");
    const hasGrid = el.shadowRoot?.querySelector(".grid, sc-search");
    expect(hasSkeleton || hasGrid).toBeTruthy();
    el.remove();
  });
});

describe("sc-sessions-view", () => {
  it("renders session list panel", async () => {
    const el = createView("sc-sessions-view");
    await el.updateComplete;
    const panel = el.shadowRoot?.querySelector(".session-list-panel");
    expect(panel).toBeTruthy();
    el.remove();
  });
});

describe("sc-config-view", () => {
  it("renders skeleton while loading", async () => {
    const el = createView("sc-config-view");
    await el.updateComplete;
    const skeletons = el.shadowRoot?.querySelectorAll("sc-skeleton");
    expect(skeletons?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });
});

describe("sc-automations-view", () => {
  it("renders content after load", async () => {
    const el = createView("sc-automations-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-skills-view", () => {
  it("renders content after load", async () => {
    const el = createView("sc-skills-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-voice-view", () => {
  it("renders voice UI content", async () => {
    const el = createView("sc-voice-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-nodes-view", () => {
  it("renders content after load", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });

  it("renders page hero with heading", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("sc-page-hero");
    expect(hero).toBeTruthy();
    const header = el.shadowRoot?.querySelector("sc-section-header");
    expect(header?.getAttribute("heading")).toBe("Nodes");
    el.remove();
  });

  it("renders nodes grid or empty state", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    const grid = el.shadowRoot?.querySelector(".nodes-grid");
    const empty = el.shadowRoot?.querySelector("sc-empty-state");
    expect(grid || empty).toBeTruthy();
    el.remove();
  });

  it("renders refresh button with aria-label", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector('sc-button[aria-label="Refresh nodes"]');
    expect(btn).toBeTruthy();
    el.remove();
  });

  it("shows empty state when no nodes loaded", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    const emptyState = el.shadowRoot?.querySelector("sc-empty-state");
    expect(emptyState).toBeTruthy();
    el.remove();
  });

  it("has scoped CSS via adoptedStyleSheets or style elements", async () => {
    const el = createView("sc-nodes-view");
    await el.updateComplete;
    const hasAdopted = (el.shadowRoot?.adoptedStyleSheets?.length ?? 0) > 0;
    const hasStyle = (el.shadowRoot?.querySelectorAll("style")?.length ?? 0) > 0;
    expect(hasAdopted || hasStyle).toBe(true);
    el.remove();
  });
});

describe("sc-usage-view", () => {
  it("renders content after load", async () => {
    const el = createView("sc-usage-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-security-view", () => {
  it("renders content after load", async () => {
    const el = createView("sc-security-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-logs-view", () => {
  it("renders log area", async () => {
    const el = createView("sc-logs-view");
    await el.updateComplete;
    const logCard = el.shadowRoot?.querySelector("sc-card.log-card, .log-area");
    expect(logCard).toBeTruthy();
    el.remove();
  });
});

/* ── Accessibility: all views ─────────────────────────────── */

describe("view accessibility", () => {
  const ALL_VIEWS = [
    "sc-overview-view",
    "sc-chat-view",
    "sc-agents-view",
    "sc-sessions-view",
    "sc-models-view",
    "sc-config-view",
    "sc-tools-view",
    "sc-channels-view",
    "sc-automations-view",
    "sc-skills-view",
    "sc-voice-view",
    "sc-nodes-view",
    "sc-usage-view",
    "sc-security-view",
    "sc-logs-view",
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

describe("sc-overview-view deep", () => {
  it("renders page hero or skeleton on load", async () => {
    const el = createView("sc-overview-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("sc-page-hero");
    const skel = el.shadowRoot?.querySelector("sc-skeleton");
    expect(hero || skel).toBeTruthy();
    el.remove();
  });

  it("renders stat cards or skeleton", async () => {
    const el = createView("sc-overview-view");
    await el.updateComplete;
    const stats = el.shadowRoot?.querySelectorAll("sc-stat-card, sc-skeleton");
    expect(stats?.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });
});

describe("sc-tools-view deep", () => {
  it("has page hero or skeleton on load", async () => {
    const el = createView("sc-tools-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("sc-page-hero, sc-skeleton");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("sc-channels-view deep", () => {
  it("has page hero or skeleton on load", async () => {
    const el = createView("sc-channels-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("sc-page-hero, sc-skeleton");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("sc-security-view deep", () => {
  it("has content on initial render", async () => {
    const el = createView("sc-security-view");
    await el.updateComplete;
    expect(el.shadowRoot?.children.length).toBeGreaterThan(0);
    el.remove();
  });
});

describe("sc-sessions-view deep", () => {
  it("has session panel or skeleton", async () => {
    const el = createView("sc-sessions-view");
    await el.updateComplete;
    const panel = el.shadowRoot?.querySelector(".session-list-panel, .sessions, sc-skeleton");
    expect(panel).toBeTruthy();
    el.remove();
  });
});

describe("sc-automations-view deep", () => {
  it("has page hero", async () => {
    const el = createView("sc-automations-view");
    await el.updateComplete;
    const hero = el.shadowRoot?.querySelector("sc-page-hero");
    expect(hero).toBeTruthy();
    el.remove();
  });
});

describe("sc-logs-view deep", () => {
  it("has log controls or card", async () => {
    const el = createView("sc-logs-view");
    await el.updateComplete;
    const controls = el.shadowRoot?.querySelector(
      ".controls, .filters, sc-segmented-control, .log-area, sc-card",
    );
    expect(controls).toBeTruthy();
    el.remove();
  });
});
