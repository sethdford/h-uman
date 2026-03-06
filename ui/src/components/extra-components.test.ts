import { describe, it, expect } from "vitest";

import "./floating-mic.js";
import "./sidebar.js";
import "./command-palette.js";
import "./sc-welcome.js";
import "./sc-sparkline.js";
import "./sc-animated-icon.js";
import "./sc-animated-number.js";
import "./sc-activity-feed.js";
import "./sc-thinking.js";
import "./sc-tool-result.js";
import "./sc-message-stream.js";
import "./sc-message-branch.js";

describe("sc-floating-mic", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-floating-mic")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-floating-mic");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("sc-sidebar", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-sidebar")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-sidebar");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("sc-command-palette", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-command-palette")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-command-palette");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("sc-welcome", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-welcome")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-welcome");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("sc-sparkline", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-sparkline")).toBeDefined();
  });

  it("should render with default props", () => {
    const el = document.createElement("sc-sparkline") as HTMLElement & {
      data: number[];
      width: number;
      height: number;
    };
    expect(el.data).toEqual([]);
    expect(el.width).toBe(80);
    expect(el.height).toBe(28);
  });

  it("should accept data array", () => {
    const el = document.createElement("sc-sparkline") as HTMLElement & { data: number[] };
    el.data = [1, 5, 3, 8, 2];
    expect(el.data).toEqual([1, 5, 3, 8, 2]);
  });
});

describe("sc-animated-icon", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-animated-icon")).toBeDefined();
  });

  it("should have default icon and state", () => {
    const el = document.createElement("sc-animated-icon") as HTMLElement & {
      icon: string;
      state: string;
    };
    expect(el.icon).toBe("check");
    expect(el.state).toBe("idle");
  });
});

describe("sc-animated-number", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-animated-number")).toBeDefined();
  });

  it("should have default value 0", () => {
    const el = document.createElement("sc-animated-number") as HTMLElement & {
      value: number;
      suffix: string;
      prefix: string;
    };
    expect(el.value).toBe(0);
    expect(el.suffix).toBe("");
    expect(el.prefix).toBe("");
  });

  it("should accept value property", () => {
    const el = document.createElement("sc-animated-number") as HTMLElement & { value: number };
    el.value = 42;
    expect(el.value).toBe(42);
  });
});

describe("sc-activity-feed", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-activity-feed")).toBeDefined();
  });

  it("should have default empty events and max 6", () => {
    const el = document.createElement("sc-activity-feed") as HTMLElement & {
      events: unknown[];
      max: number;
    };
    expect(el.events).toEqual([]);
    expect(el.max).toBe(6);
  });
});

describe("sc-thinking", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-thinking")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("sc-thinking") as HTMLElement & {
      active: boolean;
      steps: string[];
      expanded: boolean;
      duration: number;
    };
    expect(el.active).toBe(false);
    expect(el.steps).toEqual([]);
    expect(el.expanded).toBe(false);
    expect(el.duration).toBe(0);
  });

  it("shows active state", async () => {
    const el = document.createElement("sc-thinking") as HTMLElement & {
      active: boolean;
      updateComplete: Promise<boolean>;
    };
    el.active = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector(".header");
    expect(header?.textContent).toContain("Thinking");
    el.remove();
  });

  it("toggles expanded on click", async () => {
    const el = document.createElement("sc-thinking") as HTMLElement & {
      steps: string[];
      expanded: boolean;
      updateComplete: Promise<boolean>;
    };
    el.steps = ["Step 1", "Step 2"];
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector("[role='button']") as HTMLElement | null;
    header?.click();
    await el.updateComplete;
    expect(el.expanded).toBe(true);
    const stepList = el.shadowRoot?.querySelector(".steps");
    expect(stepList).toBeTruthy();
    el.remove();
  });

  it("has correct aria attributes", async () => {
    const el = document.createElement("sc-thinking") as HTMLElement & {
      expanded: boolean;
      updateComplete: Promise<boolean>;
    };
    el.expanded = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector("[role='button']");
    expect(header?.getAttribute("aria-expanded")).toBe("false");
    expect(header?.getAttribute("tabindex")).toBe("0");
    el.remove();
  });
});

describe("sc-tool-result", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-tool-result")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("sc-tool-result") as HTMLElement & {
      tool: string;
      status: string;
      content: string;
      collapsed: boolean;
    };
    expect(el.tool).toBe("");
    expect(el.status).toBe("running");
    expect(el.content).toBe("");
    expect(el.collapsed).toBe(false);
  });

  it("renders tool name", async () => {
    const el = document.createElement("sc-tool-result") as HTMLElement & {
      tool: string;
      status: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.tool = "shell";
    el.status = "success";
    el.content = "output here";
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector(".header");
    expect(header?.textContent).toContain("shell");
    el.remove();
  });

  it("shows status indicator", async () => {
    const el = document.createElement("sc-tool-result") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    el.status = "error";
    document.body.appendChild(el);
    await el.updateComplete;
    const indicator = el.shadowRoot?.querySelector(".icon");
    expect(indicator).toBeTruthy();
    el.remove();
  });

  it("toggles collapsed", async () => {
    const el = document.createElement("sc-tool-result") as HTMLElement & {
      content: string;
      collapsed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content = "some output";
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector("[role='button']") as HTMLElement | null;
    header?.click();
    await el.updateComplete;
    expect(el.collapsed).toBe(true);
    el.remove();
  });
});

describe("sc-message-stream", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-message-stream")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      streaming: boolean;
      role: string;
    };
    expect(el.content).toBe("");
    expect(el.streaming).toBe(false);
    expect(el.role).toBe("assistant");
  });

  it("renders content", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      role: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "Hello **world**";
    el.role = "assistant";
    document.body.appendChild(el);
    await el.updateComplete;
    const content = el.shadowRoot?.querySelector(".content");
    expect(content?.innerHTML).toContain("world");
    el.remove();
  });

  it("shows cursor when streaming", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      streaming: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content = "Partial";
    el.streaming = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const cursor = el.shadowRoot?.querySelector(".cursor");
    expect(cursor).toBeTruthy();
    el.remove();
  });
});

describe("sc-message-branch", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-message-branch")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("sc-message-branch") as HTMLElement & {
      branches: number;
      current: number;
    };
    expect(el.branches).toBe(1);
    expect(el.current).toBe(0);
  });

  it("renders branch count", async () => {
    const el = document.createElement("sc-message-branch") as HTMLElement & {
      branches: number;
      current: number;
      updateComplete: Promise<boolean>;
    };
    el.branches = 3;
    el.current = 1; // 0-indexed; displays as "2 / 3"
    document.body.appendChild(el);
    await el.updateComplete;
    const label = el.shadowRoot?.querySelector(".label");
    expect(label?.textContent).toContain("2");
    expect(label?.textContent).toContain("3");
    el.remove();
  });

  it("fires branch-change event", async () => {
    const el = document.createElement("sc-message-branch") as HTMLElement & {
      branches: number;
      current: number;
      updateComplete: Promise<boolean>;
    };
    el.branches = 3;
    el.current = 1;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("branch-change", ((e: CustomEvent) => {
      fired = true;
      expect(e.detail.branch).toBe(2);
    }) as EventListener);
    const nextBtn = el.shadowRoot?.querySelector(
      'button[aria-label="Next branch"]',
    ) as HTMLElement | null;
    nextBtn?.click();
    await el.updateComplete;
    expect(fired).toBe(true);
    el.remove();
  });

  it("keyboard navigation", async () => {
    const el = document.createElement("sc-message-branch") as HTMLElement & {
      branches: number;
      current: number;
      updateComplete: Promise<boolean>;
    };
    el.branches = 3;
    el.current = 2;
    document.body.appendChild(el);
    await el.updateComplete;
    let newBranch = 0;
    el.addEventListener("branch-change", ((e: CustomEvent) => {
      newBranch = e.detail.branch;
    }) as EventListener);
    const pill = el.shadowRoot?.querySelector(".pill");
    pill?.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }));
    await el.updateComplete;
    expect(newBranch).toBe(1);
    el.remove();
  });
});
