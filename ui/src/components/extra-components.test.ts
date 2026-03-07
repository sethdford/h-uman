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
import "./sc-code-block.js";
import "./sc-latex.js";
import "./sc-message-stream.js";
import "./sc-message-branch.js";
import "./sc-reasoning-block.js";
import "./sc-shortcut-overlay.js";
import "./sc-context-menu.js";

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
