import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
// sc-composer: see sc-composer.test.ts

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
import "./sc-code-block.js";
import "./sc-latex.js";
import "./sc-message-stream.js";
import "./sc-message-branch.js";
import "./sc-reasoning-block.js";
import "./sc-shortcut-overlay.js";
import "./sc-context-menu.js";
import "./sc-error-boundary.js";
import "./sc-welcome-card.js";
import "./sc-message-actions.js";
import "./sc-chat-sessions-panel.js";
import "./sc-file-preview.js";
import "./sc-stat-card.js";
import "./sc-section-header.js";
import "./sc-metric-row.js";
import "./sc-timeline.js";
import "./sc-sparkline-enhanced.js";
import "./sc-page-hero.js";
import "./sc-schedule-builder.js";
import "./sc-automation-card.js";
import "./sc-chat-bubble.js";
import "./sc-typing-indicator.js";
import "./sc-delivery-status.js";

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

  it("should render SVG with data", async () => {
    const el = document.createElement("sc-sparkline") as HTMLElement & {
      data: number[];
      updateComplete: Promise<boolean>;
    };
    el.data = [5, 10, 3, 8, 12];
    document.body.appendChild(el);
    await el.updateComplete;
    const svg = el.shadowRoot?.querySelector("svg");
    expect(svg).toBeTruthy();
    el.remove();
  });

  it("should render fill area when fill is true", async () => {
    const el = document.createElement("sc-sparkline") as HTMLElement & {
      data: number[];
      fill: boolean;
      updateComplete: Promise<boolean>;
    };
    el.data = [1, 2, 3];
    el.fill = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const area = el.shadowRoot?.querySelector(".area");
    expect(area).toBeTruthy();
    el.remove();
  });

  it("should not render fill area when fill is false", async () => {
    const el = document.createElement("sc-sparkline") as HTMLElement & {
      data: number[];
      fill: boolean;
      updateComplete: Promise<boolean>;
    };
    el.data = [1, 2, 3];
    el.fill = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const area = el.shadowRoot?.querySelector(".area");
    expect(area).toBeNull();
    el.remove();
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

describe("sc-code-block", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-code-block")).toBeDefined();
  });

  it("renders with plain code when no language", async () => {
    const el = document.createElement("sc-code-block") as HTMLElement & {
      code: string;
      language: string;
      updateComplete: Promise<boolean>;
    };
    el.code = "const x = 1;";
    el.language = "";
    document.body.appendChild(el);
    await el.updateComplete;
    const langLabel = el.shadowRoot?.querySelector(".lang-label");
    expect(langLabel?.textContent).toBe("plain");
    const codeEl = el.shadowRoot?.querySelector("code");
    expect(codeEl?.textContent).toContain("const x = 1;");
    el.remove();
  });

  it("renders with language label", async () => {
    const el = document.createElement("sc-code-block") as HTMLElement & {
      code: string;
      language: string;
      updateComplete: Promise<boolean>;
    };
    el.code = "print('hi')";
    el.language = "python";
    document.body.appendChild(el);
    await el.updateComplete;
    const langLabel = el.shadowRoot?.querySelector(".lang-label");
    expect(langLabel?.textContent).toBe("python");
    el.remove();
  });

  it("copy button is present and labeled", async () => {
    const el = document.createElement("sc-code-block") as HTMLElement & {
      code: string;
      updateComplete: Promise<boolean>;
    };
    el.code = "test";
    document.body.appendChild(el);
    await el.updateComplete;
    const copyBtn = el.shadowRoot?.querySelector(".copy-btn");
    expect(copyBtn).toBeTruthy();
    expect(copyBtn?.getAttribute("aria-label")).toBe("Copy code");
    expect(copyBtn?.textContent).toContain("Copy");
    el.remove();
  });

  it("renders code content correctly", async () => {
    const el = document.createElement("sc-code-block") as HTMLElement & {
      code: string;
      updateComplete: Promise<boolean>;
    };
    el.code = "function foo() { return 42; }";
    document.body.appendChild(el);
    await el.updateComplete;
    const content = el.shadowRoot?.querySelector(".content");
    expect(content?.textContent).toContain("function foo()");
    expect(content?.textContent).toContain("42");
    el.remove();
  });
});

describe("sc-latex", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-latex")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("sc-latex") as HTMLElement & {
      latex: string;
      display: boolean;
    };
    expect(el.latex).toBe("");
    expect(el.display).toBe(false);
  });

  it("renders raw latex before KaTeX loads", async () => {
    const el = document.createElement("sc-latex") as HTMLElement & {
      latex: string;
      updateComplete: Promise<boolean>;
    };
    el.latex = "E = mc^2";
    document.body.appendChild(el);
    await el.updateComplete;
    const span = el.shadowRoot?.querySelector(".latex-raw, .katex");
    expect(span).toBeTruthy();
    expect(el.shadowRoot?.innerHTML).toContain("E = mc^2");
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

  it("renders markdown headings", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "## Section Title";
    document.body.appendChild(el);
    await el.updateComplete;
    const h2 = el.shadowRoot?.querySelector("h2.md-heading");
    expect(h2).toBeTruthy();
    expect(h2?.textContent).toContain("Section Title");
    el.remove();
  });

  it("renders markdown code blocks via sc-code-block", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "```js\nconst x = 1;\n```";
    document.body.appendChild(el);
    await el.updateComplete;
    const codeBlock = el.shadowRoot?.querySelector("sc-code-block");
    expect(codeBlock).toBeTruthy();
    expect((codeBlock as { code: string }).code).toContain("const x = 1;");
    el.remove();
  });

  it("renders markdown lists", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "- Item one\n- Item two";
    document.body.appendChild(el);
    await el.updateComplete;
    const ul = el.shadowRoot?.querySelector("ul.md-list");
    expect(ul).toBeTruthy();
    const items = el.shadowRoot?.querySelectorAll("li.md-list-item");
    expect(items?.length).toBeGreaterThanOrEqual(2);
    el.remove();
  });

  it("renders markdown links", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "[Click here](https://example.com)";
    document.body.appendChild(el);
    await el.updateComplete;
    const link = el.shadowRoot?.querySelector("a[href='https://example.com']");
    expect(link).toBeTruthy();
    expect(link?.getAttribute("target")).toBe("_blank");
    expect(link?.textContent).toContain("Click here");
    el.remove();
  });

  it("renders inline bold, italic, code", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "**bold** _italic_ `code`";
    document.body.appendChild(el);
    await el.updateComplete;
    const content = el.shadowRoot?.querySelector(".content");
    expect(content?.querySelector("strong")).toBeTruthy();
    expect(content?.querySelector("em")).toBeTruthy();
    expect(content?.querySelector("code.inline")).toBeTruthy();
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

  it("does not show cursor when not streaming", async () => {
    const el = document.createElement("sc-message-stream") as HTMLElement & {
      content: string;
      streaming: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content = "Complete text";
    el.streaming = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const cursor = el.shadowRoot?.querySelector(".cursor");
    expect(cursor).toBeNull();
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

describe("sc-reasoning-block", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-reasoning-block")).toBeDefined();
  });

  it("renders with content", async () => {
    const el = document.createElement("sc-reasoning-block") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "The user asked about X. I will consider Y and Z.";
    document.body.appendChild(el);
    await el.updateComplete;
    const block = el.shadowRoot?.querySelector(".reasoning-block");
    expect(block).toBeTruthy();
    const preview = el.shadowRoot?.querySelector(".preview");
    expect(preview?.textContent).toContain("The user asked about X");
    el.remove();
  });

  it("starts collapsed by default", async () => {
    const el = document.createElement("sc-reasoning-block") as HTMLElement & {
      content: string;
      collapsed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content = "Some reasoning content here.";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.collapsed).toBe(true);
    const content = el.shadowRoot?.querySelector(".content");
    expect(content?.classList.contains("collapsed")).toBe(true);
    el.remove();
  });

  it("header has aria-expanded=false when collapsed", async () => {
    const el = document.createElement("sc-reasoning-block") as HTMLElement & {
      collapsed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.collapsed = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector(".header");
    expect(header?.getAttribute("aria-expanded")).toBe("false");
    el.remove();
  });

  it("clicking header toggles expanded", async () => {
    const el = document.createElement("sc-reasoning-block") as HTMLElement & {
      content: string;
      collapsed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content = "Reasoning text.";
    el.collapsed = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const header = el.shadowRoot?.querySelector(".header") as HTMLElement | null;
    header?.click();
    await el.updateComplete;
    expect(el.collapsed).toBe(false);
    expect(header?.getAttribute("aria-expanded")).toBe("true");
    header?.click();
    await el.updateComplete;
    expect(el.collapsed).toBe(true);
    el.remove();
  });

  it("shows preview when collapsed", async () => {
    const el = document.createElement("sc-reasoning-block") as HTMLElement & {
      content: string;
      collapsed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.content =
      "This is a very long reasoning block that exceeds one hundred characters and should be truncated with an ellipsis when displayed as a preview in the collapsed state.";
    el.collapsed = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const preview = el.shadowRoot?.querySelector(".preview");
    expect(preview).toBeTruthy();
    expect(preview?.textContent?.endsWith("...")).toBe(true);
    expect(preview?.textContent?.length).toBeLessThanOrEqual(103);
    el.remove();
  });
});

describe("sc-shortcut-overlay", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-shortcut-overlay")).toBeDefined();
  });

  it("should default to closed", async () => {
    const el = document.createElement("sc-shortcut-overlay") as HTMLElement & {
      open: boolean;
    };
    expect(el.open).toBe(false);
  });

  it("renders when open", async () => {
    const el = document.createElement("sc-shortcut-overlay") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const title = el.shadowRoot?.querySelector(".title");
    expect(title?.textContent).toBe("Keyboard Shortcuts");
    const kbd = el.shadowRoot?.querySelectorAll("kbd");
    expect(kbd?.length).toBeGreaterThan(0);
    el.remove();
  });

  it("lists keyboard shortcuts by category", async () => {
    const el = document.createElement("sc-shortcut-overlay") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const categories = el.shadowRoot?.querySelectorAll(".category-title");
    expect(categories?.length).toBeGreaterThanOrEqual(2);
    expect(Array.from(categories ?? []).some((c) => c.textContent?.includes("Navigation"))).toBe(
      true,
    );
    expect(Array.from(categories ?? []).some((c) => c.textContent?.includes("Chat"))).toBe(true);
    el.remove();
  });

  it("has role dialog and aria-modal", async () => {
    const el = document.createElement("sc-shortcut-overlay") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const backdrop = el.shadowRoot?.querySelector(".backdrop");
    expect(backdrop?.getAttribute("role")).toBe("dialog");
    expect(backdrop?.getAttribute("aria-modal")).toBe("true");
    expect(backdrop?.getAttribute("aria-label")).toBe("Keyboard shortcuts");
    el.remove();
  });

  it("fires close on Escape", async () => {
    const el = document.createElement("sc-shortcut-overlay") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let closed = false;
    el.addEventListener("close", () => (closed = true));
    el.shadowRoot
      ?.querySelector(".backdrop")
      ?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    expect(closed).toBe(true);
    el.remove();
  });
});

describe("sc-error-boundary", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-error-boundary")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-error-boundary");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should show fallback when error is set", async () => {
    const el = document.createElement("sc-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = new Error("Test error");
    document.body.appendChild(el);
    await el.updateComplete;
    const heading = el.shadowRoot?.querySelector(".heading");
    expect(heading?.textContent).toBe("Something went wrong");
    const btn = el.shadowRoot?.querySelector("sc-button");
    expect(btn?.textContent?.trim()).toBe("Try again");
    el.remove();
  });

  it("should render slot when no error", async () => {
    const el = document.createElement("sc-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = null;
    const span = document.createElement("span");
    span.textContent = "content";
    el.appendChild(span);
    document.body.appendChild(el);
    await el.updateComplete;
    const slot = el.shadowRoot?.querySelector(".slot");
    expect(slot).toBeTruthy();
    const fallback = el.shadowRoot?.querySelector(".fallback");
    expect(fallback).toBeNull();
    el.remove();
  });

  it("should fire retry event when Try again is clicked", async () => {
    const el = document.createElement("sc-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = new Error("Test");
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("retry", () => (fired = true));
    const btn = el.shadowRoot?.querySelector("sc-button") as HTMLElement | null;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("should have role alert when showing fallback", async () => {
    const el = document.createElement("sc-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = new Error("Test");
    document.body.appendChild(el);
    await el.updateComplete;
    const fallback = el.shadowRoot?.querySelector(".fallback");
    expect(fallback?.getAttribute("role")).toBe("alert");
    el.remove();
  });
});

describe("sc-welcome-card", () => {
  const ONBOARDED_KEY = "sc-onboarded";
  const storage: Record<string, string> = {};

  beforeEach(() => {
    Object.keys(storage).forEach((k) => delete storage[k]);
    vi.stubGlobal("localStorage", {
      getItem: (key: string) => storage[key] ?? null,
      setItem: (key: string, value: string) => {
        storage[key] = value;
      },
      removeItem: (key: string) => {
        delete storage[key];
      },
      clear: () => {
        Object.keys(storage).forEach((k) => delete storage[k]);
      },
      length: 0,
      key: () => null,
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-welcome-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-welcome-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should have default visible true and userName empty", () => {
    const el = document.createElement("sc-welcome-card") as HTMLElement & {
      visible: boolean;
      userName: string;
    };
    expect(el.visible).toBe(true);
    expect(el.userName).toBe("");
  });

  it("should show welcome content when not onboarded", async () => {
    const el = document.createElement("sc-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const heading = el.shadowRoot?.querySelector(".hero h2");
    expect(heading?.textContent).toContain("Welcome to SeaClaw");
    const desc = el.shadowRoot?.querySelector(".hero p");
    expect(desc?.textContent).toContain("autonomous AI assistant runtime");
    const features = el.shadowRoot?.querySelectorAll(".feature");
    expect(features?.length).toBe(3);
    const cta = el.shadowRoot?.querySelector(".cta sc-button");
    expect(cta?.textContent?.trim()).toBe("Get Started");
    el.remove();
  });

  it("should not render when onboarded", async () => {
    localStorage.setItem(ONBOARDED_KEY, "true");
    const el = document.createElement("sc-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const card = el.shadowRoot?.querySelector("sc-card");
    expect(card).toBeNull();
    el.remove();
  });

  it("should fire dismiss and set onboarded when Get Started clicked", async () => {
    const el = document.createElement("sc-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    let dismissFired = false;
    el.addEventListener("dismiss", () => (dismissFired = true));
    const btn = el.shadowRoot?.querySelector(".cta sc-button") as HTMLElement | null;
    btn?.click();
    await el.updateComplete;
    expect(dismissFired).toBe(true);
    expect(localStorage.getItem(ONBOARDED_KEY)).toBe("true");
    el.remove();
  });

  it("should show userName in greeting when provided", async () => {
    const el = document.createElement("sc-welcome-card") as HTMLElement & {
      userName: string;
      updateComplete: Promise<boolean>;
    };
    el.userName = "Alex";
    document.body.appendChild(el);
    await el.updateComplete;
    const heading = el.shadowRoot?.querySelector(".hero h2");
    expect(heading?.textContent).toContain("Alex");
    el.remove();
  });
});

describe("sc-message-actions", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-message-actions")).toBeDefined();
  });

  it("renders copy button", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "user";
    el.content = "test";
    document.body.appendChild(el);
    await el.updateComplete;
    const copyBtn = el.shadowRoot?.querySelector('button[aria-label="Copy"]');
    expect(copyBtn).toBeTruthy();
    el.remove();
  });

  it("renders retry button for user role", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "user";
    el.content = "test";
    document.body.appendChild(el);
    await el.updateComplete;
    const retryBtn = el.shadowRoot?.querySelector('button[aria-label="Retry"]');
    expect(retryBtn).toBeTruthy();
    el.remove();
  });

  it("renders regenerate button for assistant role", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "assistant";
    el.content = "response";
    document.body.appendChild(el);
    await el.updateComplete;
    const regenBtn = el.shadowRoot?.querySelector('button[aria-label="Regenerate"]');
    expect(regenBtn).toBeTruthy();
    el.remove();
  });

  it("fires sc-retry when retry clicked", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "user";
    el.content = "hello";
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { content: string; index: number } | null = null;
    el.addEventListener("sc-retry", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const retryBtn = el.shadowRoot?.querySelector('button[aria-label="Retry"]') as HTMLElement;
    retryBtn?.click();
    expect(detail).toEqual({ content: "hello", index: -1 });
    el.remove();
  });

  it("fires sc-copy when copy clicked", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "assistant";
    el.content = "text to copy";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("sc-copy", () => {
      fired = true;
    });
    const copyBtn = el.shadowRoot?.querySelector('button[aria-label="Copy"]') as HTMLElement;
    copyBtn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("fires sc-regenerate when regenerate clicked", async () => {
    const el = document.createElement("sc-message-actions") as HTMLElement & {
      role: string;
      content: string;
      index: number;
      updateComplete: Promise<boolean>;
    };
    el.role = "assistant";
    el.content = "response";
    el.index = 1;
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { content: string; index: number } | null = null;
    el.addEventListener("sc-regenerate", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const regenBtn = el.shadowRoot?.querySelector('button[aria-label="Regenerate"]') as HTMLElement;
    regenBtn?.click();
    expect(detail).toEqual({ content: "response", index: 1 });
    el.remove();
  });
});

describe("sc-chat-sessions-panel", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-chat-sessions-panel")).toBeDefined();
  });

  it("renders new chat button", async () => {
    const el = document.createElement("sc-chat-sessions-panel") as HTMLElement & {
      sessions: unknown[];
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector(".new-chat-btn");
    expect(btn).toBeTruthy();
    expect(btn?.textContent).toContain("New Chat");
    el.remove();
  });

  it("renders sessions", async () => {
    const el = document.createElement("sc-chat-sessions-panel") as HTMLElement & {
      sessions: Array<{ id: string; title: string; ts: number; active: boolean }>;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.sessions = [
      { id: "s1", title: "Session 1", ts: Date.now(), active: true },
      { id: "s2", title: "Session 2", ts: Date.now() - 3600000, active: false },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    const items = el.shadowRoot?.querySelectorAll(".session-item");
    expect(items?.length).toBe(2);
    expect(items?.[0]?.textContent).toContain("Session 1");
    el.remove();
  });

  it("fires sc-session-select when session clicked", async () => {
    const el = document.createElement("sc-chat-sessions-panel") as HTMLElement & {
      sessions: Array<{ id: string; title: string; ts: number; active: boolean }>;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.sessions = [{ id: "s1", title: "S1", ts: Date.now(), active: false }];
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { id: string } | null = null;
    el.addEventListener("sc-session-select", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const item = el.shadowRoot?.querySelector(".session-item") as HTMLElement;
    item?.click();
    expect(detail).toEqual({ id: "s1" });
    el.remove();
  });

  it("fires sc-session-delete when delete clicked", async () => {
    const el = document.createElement("sc-chat-sessions-panel") as HTMLElement & {
      sessions: Array<{ id: string; title: string; ts: number; active: boolean }>;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.sessions = [{ id: "s1", title: "S1", ts: Date.now(), active: false }];
    document.body.appendChild(el);
    await el.updateComplete;
    const sessionItem = el.shadowRoot?.querySelector(".session-item") as HTMLElement;
    sessionItem?.focus();
    const delBtn = el.shadowRoot?.querySelector(".delete-btn") as HTMLElement;
    if (delBtn) {
      delBtn.style.setProperty("opacity", "1");
      let detail: { id: string } | null = null;
      el.addEventListener("sc-session-delete", ((e: CustomEvent) => {
        detail = e.detail;
      }) as EventListener);
      delBtn.click();
      expect(detail).toEqual({ id: "s1" });
    }
    el.remove();
  });

  it("fires sc-session-new when new chat clicked", async () => {
    const el = document.createElement("sc-chat-sessions-panel") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("sc-session-new", () => {
      fired = true;
    });
    const btn = el.shadowRoot?.querySelector(".new-chat-btn") as HTMLElement;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });
});

describe("sc-file-preview", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-file-preview")).toBeDefined();
  });

  it("renders with no files", async () => {
    const el = document.createElement("sc-file-preview") as HTMLElement & {
      files: Array<{ name: string; size: number; type: string }>;
      updateComplete: Promise<boolean>;
    };
    el.files = [];
    document.body.appendChild(el);
    await el.updateComplete;
    const grid = el.shadowRoot?.querySelector(".grid");
    expect(grid).toBeNull();
    el.remove();
  });

  it("renders files", async () => {
    const el = document.createElement("sc-file-preview") as HTMLElement & {
      files: Array<{ name: string; size: number; type: string }>;
      updateComplete: Promise<boolean>;
    };
    el.files = [
      { name: "doc.pdf", size: 1024, type: "application/pdf" },
      { name: "readme.txt", size: 256, type: "text/plain" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    const cards = el.shadowRoot?.querySelectorAll(".card");
    expect(cards?.length).toBe(2);
    expect(el.shadowRoot?.textContent).toContain("doc.pdf");
    expect(el.shadowRoot?.textContent).toContain("1.0 KB");
    el.remove();
  });

  it("shows image preview when dataUrl provided", async () => {
    const el = document.createElement("sc-file-preview") as HTMLElement & {
      files: Array<{ name: string; size: number; type: string; dataUrl?: string }>;
      updateComplete: Promise<boolean>;
    };
    el.files = [
      { name: "img.png", size: 2048, type: "image/png", dataUrl: "data:image/png;base64,abc" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    const img = el.shadowRoot?.querySelector("img.card-image");
    expect(img).toBeTruthy();
    expect((img as HTMLImageElement)?.src).toContain("data:image/png");
    el.remove();
  });

  it("fires sc-file-remove when remove clicked", async () => {
    const el = document.createElement("sc-file-preview") as HTMLElement & {
      files: Array<{ name: string; size: number; type: string }>;
      updateComplete: Promise<boolean>;
    };
    el.files = [{ name: "a.txt", size: 100, type: "text/plain" }];
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { index: number } | null = null;
    el.addEventListener("sc-file-remove", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const removeBtn = el.shadowRoot?.querySelector(".remove-btn") as HTMLElement;
    removeBtn?.click();
    expect(detail).toEqual({ index: 0 });
    el.remove();
  });
});

describe("sc-context-menu", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-context-menu")).toBeDefined();
  });

  it("should default to closed", async () => {
    const el = document.createElement("sc-context-menu") as HTMLElement & {
      open: boolean;
    };
    expect(el.open).toBe(false);
  });

  it("positions at x,y when open", async () => {
    const el = document.createElement("sc-context-menu") as HTMLElement & {
      open: boolean;
      x: number;
      y: number;
      items: { label: string; action: () => void }[];
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.x = 100;
    el.y = 200;
    el.items = [{ label: "Test", action: () => {} }];
    document.body.appendChild(el);
    await el.updateComplete;
    const menu = el.shadowRoot?.querySelector(".menu");
    expect(menu).toBeTruthy();
    expect((menu as HTMLElement)?.style.left).toBe("100px");
    expect((menu as HTMLElement)?.style.top).toBe("200px");
    el.remove();
  });

  it("items are clickable", async () => {
    let actionFired = false;
    const el = document.createElement("sc-context-menu") as HTMLElement & {
      open: boolean;
      items: { label: string; action: () => void }[];
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.items = [{ label: "Do thing", action: () => (actionFired = true) }];
    document.body.appendChild(el);
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector(".item");
    (btn as HTMLElement)?.click();
    expect(actionFired).toBe(true);
    el.remove();
  });

  it("has role menu", async () => {
    const el = document.createElement("sc-context-menu") as HTMLElement & {
      open: boolean;
      items: { label: string; action: () => void }[];
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.items = [{ label: "A", action: () => {} }];
    document.body.appendChild(el);
    await el.updateComplete;
    const menu = el.shadowRoot?.querySelector(".menu");
    expect(menu?.getAttribute("role")).toBe("menu");
    el.remove();
  });
});

describe("sc-stat-card", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-stat-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-stat-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render value and label", async () => {
    const el = document.createElement("sc-stat-card") as HTMLElement & {
      value: number;
      label: string;
      updateComplete: Promise<boolean>;
    };
    el.value = 42;
    el.label = "Tests";
    document.body.appendChild(el);
    await el.updateComplete;
    const shadow = el.shadowRoot;
    expect(shadow?.textContent).toContain("Tests");
    el.remove();
  });
});

describe("sc-section-header", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-section-header")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-section-header");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render heading", async () => {
    const el = document.createElement("sc-section-header") as HTMLElement & {
      heading: string;
      updateComplete: Promise<boolean>;
    };
    el.heading = "Overview";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("Overview");
    el.remove();
  });
});

describe("sc-metric-row", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-metric-row")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-metric-row");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render items", async () => {
    const el = document.createElement("sc-metric-row") as HTMLElement & {
      items: Array<{ label: string; value: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ label: "CPU", value: "23%" }];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("CPU");
    expect(el.shadowRoot?.textContent).toContain("23%");
    el.remove();
  });
});

describe("sc-timeline", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-timeline")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-timeline");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render items", async () => {
    const el = document.createElement("sc-timeline") as HTMLElement & {
      items: Array<{ time: string; message: string; status: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ time: "2m ago", message: "Test passed", status: "success" }];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("Test passed");
    el.remove();
  });

  it("should show empty state", async () => {
    const el = document.createElement("sc-timeline") as HTMLElement & {
      items: Array<{ time: string; message: string; status: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("No recent activity");
    el.remove();
  });
});

describe("sc-sparkline-enhanced", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-sparkline-enhanced")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-sparkline-enhanced");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render SVG with data", async () => {
    const el = document.createElement("sc-sparkline-enhanced") as HTMLElement & {
      data: number[];
      updateComplete: Promise<boolean>;
    };
    el.data = [10, 20, 15, 25, 30];
    document.body.appendChild(el);
    await el.updateComplete;
    const svg = el.shadowRoot?.querySelector("svg");
    expect(svg).toBeTruthy();
    el.remove();
  });
});

describe("sc-page-hero", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-page-hero")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-page-hero");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render slot content", async () => {
    const el = document.createElement("sc-page-hero") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    el.innerHTML = "<p>Hello</p>";
    document.body.appendChild(el);
    await el.updateComplete;
    const slot = el.shadowRoot?.querySelector("slot");
    expect(slot).toBeTruthy();
    el.remove();
  });
});

describe("sc-schedule-builder", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-schedule-builder")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-schedule-builder");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with shadow DOM", async () => {
    const el = document.createElement("sc-schedule-builder") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("sc-automation-card", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-automation-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-automation-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render empty state without job", async () => {
    const el = document.createElement("sc-automation-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("sc-chat-bubble", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-chat-bubble")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-chat-bubble");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default props", async () => {
    const el = document.createElement("sc-chat-bubble") as HTMLElement & {
      content: string;
      role: "user" | "assistant";
      streaming: boolean;
      showTail: boolean;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.content).toBe("");
    expect(el.role).toBe("assistant");
    expect(el.streaming).toBe(false);
    expect(el.showTail).toBe(false);
    el.remove();
  });

  it("renders content and role=user changes output", async () => {
    const el = document.createElement("sc-chat-bubble") as HTMLElement & {
      content: string;
      role: "user" | "assistant";
      updateComplete: Promise<boolean>;
    };
    el.content = "Hello world";
    el.role = "user";
    document.body.appendChild(el);
    await el.updateComplete;
    const bubble = el.shadowRoot?.querySelector(".bubble");
    expect(bubble?.classList.contains("role-user")).toBe(true);
    expect(bubble?.textContent).toContain("Hello world");
    el.remove();
  });

  it("shows cursor when streaming", async () => {
    const el = document.createElement("sc-chat-bubble") as HTMLElement & {
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

  it("has role=article for accessibility", async () => {
    const el = document.createElement("sc-chat-bubble") as HTMLElement & {
      content: string;
      role: "user" | "assistant";
      updateComplete: Promise<boolean>;
    };
    el.content = "Test";
    el.role = "assistant";
    document.body.appendChild(el);
    await el.updateComplete;
    const bubble = el.shadowRoot?.querySelector(".bubble");
    expect(bubble?.getAttribute("role")).toBe("article");
    el.remove();
  });
});

describe("sc-typing-indicator", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-typing-indicator")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-typing-indicator");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default props", async () => {
    const el = document.createElement("sc-typing-indicator") as HTMLElement & {
      elapsed: string;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.elapsed).toBe("");
    el.remove();
  });

  it("renders elapsed time when provided", async () => {
    const el = document.createElement("sc-typing-indicator") as HTMLElement & {
      elapsed: string;
      updateComplete: Promise<boolean>;
    };
    el.elapsed = "12s";
    document.body.appendChild(el);
    await el.updateComplete;
    const elapsedSpan = el.shadowRoot?.querySelector(".elapsed");
    expect(elapsedSpan?.textContent).toBe("12s");
    el.remove();
  });

  it("has role=status for accessibility", async () => {
    const el = document.createElement("sc-typing-indicator") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const indicator = el.shadowRoot?.querySelector(".indicator");
    expect(indicator?.getAttribute("role")).toBe("status");
    el.remove();
  });
});

describe("sc-delivery-status", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("sc-delivery-status")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("sc-delivery-status");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default status", async () => {
    const el = document.createElement("sc-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.status).toBe("sent");
    el.remove();
  });

  it("status=failed shows retry button", async () => {
    const el = document.createElement("sc-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    el.status = "failed";
    document.body.appendChild(el);
    await el.updateComplete;
    const retryBtn = el.shadowRoot?.querySelector(".retry");
    expect(retryBtn).toBeTruthy();
    expect(retryBtn?.textContent).toContain("Retry");
    el.remove();
  });

  it("fires sc-retry when retry clicked", async () => {
    const el = document.createElement("sc-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    el.status = "failed";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("sc-retry", () => (fired = true));
    const retryBtn = el.shadowRoot?.querySelector(".retry") as HTMLElement | null;
    retryBtn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("has role=status for accessibility", async () => {
    const el = document.createElement("sc-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    el.status = "sending";
    document.body.appendChild(el);
    await el.updateComplete;
    const statusEl = el.shadowRoot?.querySelector("[role='status']");
    expect(statusEl).toBeTruthy();
    el.remove();
  });
});
