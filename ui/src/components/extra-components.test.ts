import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";

import type { ScChart } from "./hu-chart.js";
import type { ScJsonViewer } from "./hu-json-viewer.js";
import type { ScPagination } from "./hu-pagination.js";
import type { ScDataTableV2 } from "./hu-data-table-v2.js";
import type { ScCheckbox } from "./hu-checkbox.js";
import type { ScCombobox } from "./hu-combobox.js";
import type { ScFormGroup } from "./hu-form-group.js";

import "./floating-mic.js";
import "./sidebar.js";
import "./command-palette.js";
import "./hu-welcome.js";
import "./hu-sparkline.js";
import "./hu-animated-icon.js";
import "./hu-animated-number.js";
import "./hu-activity-feed.js";
import "./hu-thinking.js";
import "./hu-tool-result.js";
import "./hu-code-block.js";
import "./hu-artifact-viewer.js";
import "./hu-artifact-panel.js";
import "./hu-latex.js";
import "./hu-message-stream.js";
import "./hu-message-branch.js";
import "./hu-reasoning-block.js";
import "./hu-shortcut-overlay.js";
import "./hu-context-menu.js";
import "./hu-error-boundary.js";
import "./hu-welcome-card.js";
import "./hu-message-actions.js";
import "./hu-chat-sessions-panel.js";
import "./hu-file-preview.js";
import "./hu-stat-card.js";
import "./hu-stats-row.js";
import "./hu-section-header.js";
import "./hu-metric-row.js";
import "./hu-timeline.js";
import "./hu-sparkline-enhanced.js";
import "./hu-forecast-chart.js";
import "./hu-page-hero.js";
import "./hu-schedule-builder.js";
import "./hu-automation-card.js";
import "./hu-automation-form.js";
import "./hu-chat-bubble.js";
import "./hu-typing-indicator.js";
import "./hu-delivery-status.js";
import "./hu-message-group.js";
import "./hu-link-preview.js";
import "./hu-model-selector.js";
import "./hu-tapback-menu.js";
import "./hu-chat-composer.js";
import "./hu-message-thread.js";
import "./hu-branch-tree.js";
import "./hu-image-viewer.js";
import "./hu-voice-orb.js";
import "./hu-voice-conversation.js";
import "./hu-chart.js";
import "./hu-json-viewer.js";
import "./hu-pagination.js";
import "./hu-data-table-v2.js";
import "./hu-checkbox.js";
import "./hu-combobox.js";
import "./hu-form-group.js";
import "./hu-activity-timeline.js";
import "./hu-overview-stats.js";
import "./hu-sessions-table.js";
import "./hu-skill-card.js";
import "./hu-skill-detail.js";
import "./hu-skill-registry.js";
import { SPRING_PRESETS, springAnimate } from "../lib/spring.js";

describe("hu-checkbox", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-checkbox")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-checkbox");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should default checked to false", () => {
    const el = document.createElement("hu-checkbox") as ScCheckbox;
    expect(el.checked).toBe(false);
  });

  it("should toggle on click", async () => {
    const el = document.createElement("hu-checkbox") as ScCheckbox;
    document.body.appendChild(el);
    await el.updateComplete;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const events: any[] = [];
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    el.addEventListener("hu-change", (e: any) => events.push(e.detail));
    (el.shadowRoot?.querySelector('[role="checkbox"]') as HTMLElement)?.click();
    expect(events.length).toBe(1);
    expect(events[0].checked).toBe(true);
    el.remove();
  });

  it("should not toggle when disabled", async () => {
    const el = document.createElement("hu-checkbox") as ScCheckbox;
    el.disabled = true;
    document.body.appendChild(el);
    await el.updateComplete;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const events: any[] = [];
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    el.addEventListener("hu-change", (e: any) => events.push(e.detail));
    (el.shadowRoot?.querySelector('[role="checkbox"]') as HTMLElement)?.click();
    expect(events.length).toBe(0);
    el.remove();
  });

  it("should support label", () => {
    const el = document.createElement("hu-checkbox") as ScCheckbox;
    el.label = "Accept terms";
    expect(el.label).toBe("Accept terms");
  });
});

describe("hu-combobox", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-combobox")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-combobox");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept options", () => {
    const el = document.createElement("hu-combobox") as ScCombobox;
    el.options = [{ value: "a", label: "Alpha" }];
    expect(el.options.length).toBe(1);
  });

  it("should accept value", () => {
    const el = document.createElement("hu-combobox") as ScCombobox;
    el.value = "test";
    expect(el.value).toBe("test");
  });

  it("should default freeText to false", () => {
    const el = document.createElement("hu-combobox") as ScCombobox;
    expect(el.freeText).toBe(false);
  });

  it("should have combobox role", async () => {
    const el = document.createElement("hu-combobox") as ScCombobox;
    document.body.appendChild(el);
    await el.updateComplete;
    const input = el.shadowRoot?.querySelector('[role="combobox"]');
    expect(input).toBeTruthy();
    el.remove();
  });
});

describe("hu-form-group", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-form-group")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-form-group");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should default dirty to false", () => {
    const el = document.createElement("hu-form-group") as ScFormGroup;
    expect(el.dirty).toBe(false);
  });

  it("should default valid to true", () => {
    const el = document.createElement("hu-form-group") as ScFormGroup;
    expect(el.valid).toBe(true);
  });

  it("should have validate method", () => {
    const el = document.createElement("hu-form-group") as ScFormGroup;
    expect(typeof el.validate).toBe("function");
  });

  it("should have reset method", () => {
    const el = document.createElement("hu-form-group") as ScFormGroup;
    expect(typeof el.reset).toBe("function");
  });
});

describe("hu-pagination", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-pagination")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-pagination");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should compute page count from total and pageSize", () => {
    const el = document.createElement("hu-pagination") as ScPagination;
    el.total = 100;
    el.pageSize = 10;
    expect(el.pageCount).toBe(10);
  });

  it("should default page to 1", () => {
    const el = document.createElement("hu-pagination") as ScPagination;
    expect(el.page).toBe(1);
  });

  it("should default pageSize to 10", () => {
    const el = document.createElement("hu-pagination") as ScPagination;
    expect(el.pageSize).toBe(10);
  });

  it("should fire hu-page-change on page navigation", async () => {
    const el = document.createElement("hu-pagination") as ScPagination;
    el.total = 50;
    el.pageSize = 10;
    el.page = 1;
    document.body.appendChild(el);
    await el.updateComplete;

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const events: any[] = [];
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    el.addEventListener("hu-page-change", (e: any) => events.push(e.detail));

    const nextBtn = el.shadowRoot?.querySelector('[aria-label="Next page"]') as HTMLButtonElement;
    nextBtn?.click();
    expect(events.length).toBe(1);
    expect(events[0].page).toBe(2);
    el.remove();
  });

  it("should render showing label", async () => {
    const el = document.createElement("hu-pagination") as ScPagination;
    el.total = 100;
    el.pageSize = 10;
    el.page = 1;
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("1");
    expect(text).toContain("10");
    expect(text).toContain("100");
    el.remove();
  });
});

describe("hu-data-table-v2", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-data-table-v2")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-data-table-v2");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render rows", async () => {
    const el = document.createElement("hu-data-table-v2") as ScDataTableV2;
    el.columns = [{ key: "name", label: "Name" }];
    el.rows = [{ name: "Alice" }, { name: "Bob" }];
    el.paginated = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const tds = el.shadowRoot?.querySelectorAll("td");
    expect(tds?.length).toBeGreaterThanOrEqual(2);
    el.remove();
  });

  it("should fire hu-row-click on row click", async () => {
    const el = document.createElement("hu-data-table-v2") as ScDataTableV2;
    el.columns = [{ key: "name", label: "Name" }];
    el.rows = [{ name: "Alice" }];
    el.paginated = false;
    document.body.appendChild(el);
    await el.updateComplete;

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const events: any[] = [];
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    el.addEventListener("hu-row-click", (e: any) => events.push(e.detail));

    const row = el.shadowRoot?.querySelector("tbody tr") as HTMLElement;
    row?.click();
    expect(events.length).toBe(1);
    expect(events[0].row.name).toBe("Alice");
    el.remove();
  });

  it("should sort when clicking sortable column", async () => {
    const el = document.createElement("hu-data-table-v2") as ScDataTableV2;
    el.columns = [{ key: "name", label: "Name", sortable: true }];
    el.rows = [{ name: "Bob" }, { name: "Alice" }];
    el.paginated = false;
    document.body.appendChild(el);
    await el.updateComplete;

    const th = el.shadowRoot?.querySelector("th") as HTMLElement;
    th?.click();
    await el.updateComplete;

    const cells = el.shadowRoot?.querySelectorAll("td");
    expect(cells?.[0]?.textContent?.trim()).toBe("Alice");
    el.remove();
  });

  it("should show empty state for no rows", async () => {
    const el = document.createElement("hu-data-table-v2") as ScDataTableV2;
    el.columns = [{ key: "name", label: "Name" }];
    el.rows = [];
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("No data");
    el.remove();
  });

  it("should default paginated to true", () => {
    const el = document.createElement("hu-data-table-v2") as ScDataTableV2;
    expect(el.paginated).toBe(true);
  });
});

describe("hu-json-viewer", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-json-viewer")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-json-viewer");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept data property", () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = { key: "value" };
    expect(el.data).toEqual({ key: "value" });
  });

  it("should default expandedDepth to 2", () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    expect(el.expandedDepth).toBe(2);
  });

  it("should render primitive string value", async () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = "hello";
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain('"hello"');
    el.remove();
  });

  it("should render object keys", async () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = { name: "test" };
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("name");
    el.remove();
  });

  it("should render array length indicator", async () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = [1, 2, 3];
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("3");
    el.remove();
  });

  it("should render null value", async () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = null;
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("null");
    el.remove();
  });

  it("should use tree role for accessibility", async () => {
    const el = document.createElement("hu-json-viewer") as ScJsonViewer;
    el.data = { a: 1 };
    document.body.appendChild(el);
    await el.updateComplete;
    const tree = el.shadowRoot?.querySelector('[role="tree"]');
    expect(tree).toBeTruthy();
    el.remove();
  });
});

describe("hu-floating-mic", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-floating-mic")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-floating-mic");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-sidebar", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-sidebar")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-sidebar");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-command-palette", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-command-palette")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-command-palette");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-welcome", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-welcome")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-welcome");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-sparkline", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-sparkline")).toBeDefined();
  });

  it("should render with default props", () => {
    const el = document.createElement("hu-sparkline") as HTMLElement & {
      data: number[];
      width: number;
      height: number;
    };
    expect(el.data).toEqual([]);
    expect(el.width).toBe(80);
    expect(el.height).toBe(28);
  });

  it("should accept data array", () => {
    const el = document.createElement("hu-sparkline") as HTMLElement & { data: number[] };
    el.data = [1, 5, 3, 8, 2];
    expect(el.data).toEqual([1, 5, 3, 8, 2]);
  });
});

describe("hu-animated-icon", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-animated-icon")).toBeDefined();
  });

  it("should have default icon and state", () => {
    const el = document.createElement("hu-animated-icon") as HTMLElement & {
      icon: string;
      state: string;
    };
    expect(el.icon).toBe("check");
    expect(el.state).toBe("idle");
  });
});

describe("hu-animated-number", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-animated-number")).toBeDefined();
  });

  it("should have default value 0", () => {
    const el = document.createElement("hu-animated-number") as HTMLElement & {
      value: number;
      suffix: string;
      prefix: string;
    };
    expect(el.value).toBe(0);
    expect(el.suffix).toBe("");
    expect(el.prefix).toBe("");
  });

  it("should accept value property", () => {
    const el = document.createElement("hu-animated-number") as HTMLElement & { value: number };
    el.value = 42;
    expect(el.value).toBe(42);
  });
});

describe("hu-activity-feed", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-activity-feed")).toBeDefined();
  });

  it("should have default empty events and max 6", () => {
    const el = document.createElement("hu-activity-feed") as HTMLElement & {
      events: unknown[];
      max: number;
    };
    expect(el.events).toEqual([]);
    expect(el.max).toBe(6);
  });
});

describe("hu-thinking", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-thinking")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("hu-thinking") as HTMLElement & {
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
    const el = document.createElement("hu-thinking") as HTMLElement & {
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
    const el = document.createElement("hu-thinking") as HTMLElement & {
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
    const el = document.createElement("hu-thinking") as HTMLElement & {
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

describe("hu-tool-result", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-tool-result")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("hu-tool-result") as HTMLElement & {
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
    const el = document.createElement("hu-tool-result") as HTMLElement & {
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
    const el = document.createElement("hu-tool-result") as HTMLElement & {
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
    const el = document.createElement("hu-tool-result") as HTMLElement & {
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

describe("hu-code-block", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-code-block")).toBeDefined();
  });

  it("renders with plain code when no language", async () => {
    const el = document.createElement("hu-code-block") as HTMLElement & {
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
    const el = document.createElement("hu-code-block") as HTMLElement & {
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
    const el = document.createElement("hu-code-block") as HTMLElement & {
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
    const el = document.createElement("hu-code-block") as HTMLElement & {
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

describe("hu-artifact-viewer", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-artifact-viewer")).toBeDefined();
  });

  it("renders code type with hu-code-block", async () => {
    const el = document.createElement("hu-artifact-viewer") as HTMLElement & {
      type: string;
      content: string;
      language: string;
      updateComplete: Promise<boolean>;
    };
    el.type = "code";
    el.content = "const x = 1;";
    el.language = "javascript";
    document.body.appendChild(el);
    await el.updateComplete;
    const codeBlock = el.shadowRoot?.querySelector("hu-code-block");
    expect(codeBlock).toBeTruthy();
    el.remove();
  });

  it("shows type label in toolbar", async () => {
    const el = document.createElement("hu-artifact-viewer") as HTMLElement & {
      type: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.type = "document";
    el.content = "# Hello";
    document.body.appendChild(el);
    await el.updateComplete;
    const typeLabel = el.shadowRoot?.querySelector(".type-label");
    expect(typeLabel?.textContent).toBe("Document");
    el.remove();
  });

  it("renders markdown content for type=document", async () => {
    const el = document.createElement("hu-artifact-viewer") as HTMLElement & {
      type: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.type = "document";
    el.content = "# Hello **world**";
    document.body.appendChild(el);
    await el.updateComplete;
    const body = el.shadowRoot?.querySelector(".body.md-content");
    expect(body).toBeTruthy();
    expect(body?.innerHTML).toContain("Hello");
    el.remove();
  });

  it("renders iframe for type=html", async () => {
    const el = document.createElement("hu-artifact-viewer") as HTMLElement & {
      type: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.type = "html";
    el.content = "<p>Hello</p>";
    document.body.appendChild(el);
    await el.updateComplete;
    const iframe = el.shadowRoot?.querySelector("iframe.iframe-wrap");
    expect(iframe).toBeTruthy();
    expect((iframe as HTMLIFrameElement).srcdoc).toContain("Hello");
    el.remove();
  });

  it("shows copy toolbar", async () => {
    const el = document.createElement("hu-artifact-viewer") as HTMLElement & {
      type: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.type = "code";
    el.content = "x";
    document.body.appendChild(el);
    await el.updateComplete;
    const toolbar = el.shadowRoot?.querySelector(".toolbar");
    const copyBtn = el.shadowRoot?.querySelector(".copy-btn");
    expect(toolbar).toBeTruthy();
    expect(copyBtn).toBeTruthy();
    expect(copyBtn?.textContent).toContain("Copy");
    el.remove();
  });
});

describe("hu-artifact-panel", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-artifact-panel")).toBeDefined();
  });

  it("renders nothing when no artifact is provided", async () => {
    const el = document.createElement("hu-artifact-panel") as HTMLElement & {
      artifact: unknown;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.artifact = null;
    el.open = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const panelWrap = el.shadowRoot?.querySelector(".panel-wrap");
    expect(panelWrap).toBeFalsy();
    el.remove();
  });

  it("renders header with title when artifact is provided", async () => {
    const el = document.createElement("hu-artifact-panel") as HTMLElement & {
      artifact: { id: string; type: string; title: string; content: string; versions: unknown[] };
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.artifact = {
      id: "a1",
      type: "code",
      title: "Test Artifact",
      content: "const x = 1;",
      versions: [{ content: "const x = 1;", ts: Date.now() }],
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const headerTitle = el.shadowRoot?.querySelector(".header-title");
    expect(headerTitle?.textContent).toBe("Test Artifact");
    const viewer = el.shadowRoot?.querySelector("hu-artifact-viewer");
    expect(viewer).toBeTruthy();
    el.remove();
  });

  it("shows version navigation when artifact has multiple versions", async () => {
    const el = document.createElement("hu-artifact-panel") as HTMLElement & {
      artifact: {
        id: string;
        type: string;
        title: string;
        content: string;
        versions: Array<{ content: string; ts: number }>;
      };
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.artifact = {
      id: "a1",
      type: "code",
      title: "Multi-version",
      content: "v1",
      versions: [
        { content: "v1", ts: 1000 },
        { content: "v2", ts: 2000 },
        { content: "v3", ts: 3000 },
      ],
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    await el.updateComplete; // wait for _updatePanelState in updated() to set _currentVersionIndex
    const versionNav = el.shadowRoot?.querySelector(".version-nav");
    expect(versionNav).toBeTruthy();
    expect(versionNav?.textContent).toContain("3 / 3");
    el.remove();
  });

  it("dispatches hu-artifact-close event when close button is clicked", async () => {
    const el = document.createElement("hu-artifact-panel") as HTMLElement & {
      artifact: {
        id: string;
        type: string;
        title: string;
        content: string;
        versions: Array<{ content: string; ts: number }>;
      };
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.artifact = {
      id: "a1",
      type: "code",
      title: "Test",
      content: "x",
      versions: [{ content: "x", ts: Date.now() }],
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let closed = false;
    el.addEventListener("hu-artifact-close", () => {
      closed = true;
    });
    const closeBtn = el.shadowRoot?.querySelector(".close-btn") as HTMLButtonElement;
    closeBtn?.click();
    expect(closed).toBe(true);
    el.remove();
  });

  it("copy button exists in footer", async () => {
    const el = document.createElement("hu-artifact-panel") as HTMLElement & {
      artifact: {
        id: string;
        type: string;
        title: string;
        content: string;
        versions: Array<{ content: string; ts: number }>;
      };
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.artifact = {
      id: "a1",
      type: "code",
      title: "Test",
      content: "x",
      versions: [{ content: "x", ts: Date.now() }],
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const footer = el.shadowRoot?.querySelector(".footer");
    expect(footer).toBeTruthy();
    const copyBtn = footer?.querySelector("hu-button");
    expect(copyBtn).toBeTruthy();
    expect(footer?.textContent).toContain("Copy");
    el.remove();
  });
});

describe("hu-latex", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-latex")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("hu-latex") as HTMLElement & {
      latex: string;
      display: boolean;
    };
    expect(el.latex).toBe("");
    expect(el.display).toBe(false);
  });

  it("renders raw latex before KaTeX loads", async () => {
    const el = document.createElement("hu-latex") as HTMLElement & {
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

describe("hu-message-stream", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-message-stream")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("hu-message-stream") as HTMLElement & {
      content: string;
      streaming: boolean;
      role: string;
    };
    expect(el.content).toBe("");
    expect(el.streaming).toBe(false);
    expect(el.role).toBe("assistant");
  });

  it("renders content", async () => {
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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

  it("renders markdown code blocks via hu-code-block", async () => {
    const el = document.createElement("hu-message-stream") as HTMLElement & {
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.content = "```js\nconst x = 1;\n```";
    document.body.appendChild(el);
    await el.updateComplete;
    const codeBlock = el.shadowRoot?.querySelector("hu-code-block");
    expect(codeBlock).toBeTruthy();
    expect((codeBlock as { code: string }).code).toContain("const x = 1;");
    el.remove();
  });

  it("renders markdown lists", async () => {
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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
    const el = document.createElement("hu-message-stream") as HTMLElement & {
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

describe("hu-message-branch", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-message-branch")).toBeDefined();
  });

  it("should have default properties", () => {
    const el = document.createElement("hu-message-branch") as HTMLElement & {
      branches: number;
      current: number;
    };
    expect(el.branches).toBe(1);
    expect(el.current).toBe(0);
  });

  it("renders branch count", async () => {
    const el = document.createElement("hu-message-branch") as HTMLElement & {
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
    const el = document.createElement("hu-message-branch") as HTMLElement & {
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
    const el = document.createElement("hu-message-branch") as HTMLElement & {
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

describe("hu-reasoning-block", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-reasoning-block")).toBeDefined();
  });

  it("renders with content", async () => {
    const el = document.createElement("hu-reasoning-block") as HTMLElement & {
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
    const el = document.createElement("hu-reasoning-block") as HTMLElement & {
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
    const el = document.createElement("hu-reasoning-block") as HTMLElement & {
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
    const el = document.createElement("hu-reasoning-block") as HTMLElement & {
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
    const el = document.createElement("hu-reasoning-block") as HTMLElement & {
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

describe("hu-shortcut-overlay", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-shortcut-overlay")).toBeDefined();
  });

  it("should default to closed", async () => {
    const el = document.createElement("hu-shortcut-overlay") as HTMLElement & {
      open: boolean;
    };
    expect(el.open).toBe(false);
  });

  it("renders when open", async () => {
    const el = document.createElement("hu-shortcut-overlay") as HTMLElement & {
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
    const el = document.createElement("hu-shortcut-overlay") as HTMLElement & {
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
    const el = document.createElement("hu-shortcut-overlay") as HTMLElement & {
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
    const el = document.createElement("hu-shortcut-overlay") as HTMLElement & {
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

describe("hu-error-boundary", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-error-boundary")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-error-boundary");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should show fallback when error is set", async () => {
    const el = document.createElement("hu-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = new Error("Test error");
    document.body.appendChild(el);
    await el.updateComplete;
    const heading = el.shadowRoot?.querySelector(".heading");
    expect(heading?.textContent).toBe("Something went wrong");
    const btn = el.shadowRoot?.querySelector("hu-button");
    expect(btn?.textContent?.trim()).toBe("Try again");
    el.remove();
  });

  it("should render slot when no error", async () => {
    const el = document.createElement("hu-error-boundary") as HTMLElement & {
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
    const el = document.createElement("hu-error-boundary") as HTMLElement & {
      error: Error | null;
      updateComplete: Promise<boolean>;
    };
    el.error = new Error("Test");
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("retry", () => (fired = true));
    const btn = el.shadowRoot?.querySelector("hu-button") as HTMLElement | null;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("should have role alert when showing fallback", async () => {
    const el = document.createElement("hu-error-boundary") as HTMLElement & {
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

describe("hu-welcome-card", () => {
  const ONBOARDED_KEY = "hu-onboarded";
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
    expect(customElements.get("hu-welcome-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-welcome-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should have default visible true and userName empty", () => {
    const el = document.createElement("hu-welcome-card") as HTMLElement & {
      visible: boolean;
      userName: string;
    };
    expect(el.visible).toBe(true);
    expect(el.userName).toBe("");
  });

  it("should show welcome content when not onboarded", async () => {
    const el = document.createElement("hu-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const heading = el.shadowRoot?.querySelector(".hero h2");
    expect(heading?.textContent).toContain("Welcome to Human");
    const desc = el.shadowRoot?.querySelector(".hero p");
    expect(desc?.textContent).toContain("autonomous AI assistant runtime");
    const features = el.shadowRoot?.querySelectorAll(".feature");
    expect(features?.length).toBe(3);
    const cta = el.shadowRoot?.querySelector(".cta hu-button");
    expect(cta?.textContent?.trim()).toBe("Get Started");
    el.remove();
  });

  it("should not render when onboarded", async () => {
    localStorage.setItem(ONBOARDED_KEY, "true");
    const el = document.createElement("hu-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const card = el.shadowRoot?.querySelector("hu-card");
    expect(card).toBeNull();
    el.remove();
  });

  it("should fire dismiss and set onboarded when Get Started clicked", async () => {
    const el = document.createElement("hu-welcome-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    let dismissFired = false;
    el.addEventListener("dismiss", () => (dismissFired = true));
    const btn = el.shadowRoot?.querySelector(".cta hu-button") as HTMLElement | null;
    btn?.click();
    await el.updateComplete;
    expect(dismissFired).toBe(true);
    expect(localStorage.getItem(ONBOARDED_KEY)).toBe("true");
    el.remove();
  });

  it("should show userName in greeting when provided", async () => {
    const el = document.createElement("hu-welcome-card") as HTMLElement & {
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

describe("hu-message-actions", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-message-actions")).toBeDefined();
  });

  it("renders copy button", async () => {
    const el = document.createElement("hu-message-actions") as HTMLElement & {
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
    const el = document.createElement("hu-message-actions") as HTMLElement & {
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
    const el = document.createElement("hu-message-actions") as HTMLElement & {
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

  it("fires hu-retry when retry clicked", async () => {
    const el = document.createElement("hu-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "user";
    el.content = "hello";
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { content: string; index: number } | null = null;
    el.addEventListener("hu-retry", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const retryBtn = el.shadowRoot?.querySelector('button[aria-label="Retry"]') as HTMLElement;
    retryBtn?.click();
    expect(detail).toEqual({ content: "hello", index: -1 });
    el.remove();
  });

  it("fires hu-copy when copy clicked", async () => {
    const el = document.createElement("hu-message-actions") as HTMLElement & {
      role: string;
      content: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "assistant";
    el.content = "text to copy";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-copy", () => {
      fired = true;
    });
    const copyBtn = el.shadowRoot?.querySelector('button[aria-label="Copy"]') as HTMLElement;
    copyBtn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("fires hu-regenerate when regenerate clicked", async () => {
    const el = document.createElement("hu-message-actions") as HTMLElement & {
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
    el.addEventListener("hu-regenerate", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const regenBtn = el.shadowRoot?.querySelector('button[aria-label="Regenerate"]') as HTMLElement;
    regenBtn?.click();
    expect(detail).toEqual({ content: "response", index: 1 });
    el.remove();
  });
});

describe("hu-chat-sessions-panel", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-chat-sessions-panel")).toBeDefined();
  });

  it("renders new chat button", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
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
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
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

  it("fires hu-session-select when session clicked", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
      sessions: Array<{ id: string; title: string; ts: number; active: boolean }>;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.sessions = [{ id: "s1", title: "S1", ts: Date.now(), active: false }];
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { id: string } | null = null;
    el.addEventListener("hu-session-select", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const item = el.shadowRoot?.querySelector(".session-item") as HTMLElement;
    item?.click();
    expect(detail).toEqual({ id: "s1" });
    el.remove();
  });

  it("fires hu-session-delete when delete clicked", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
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
      el.addEventListener("hu-session-delete", ((e: CustomEvent) => {
        detail = e.detail;
      }) as EventListener);
      delBtn.click();
      expect(detail).toEqual({ id: "s1" });
    }
    el.remove();
  });

  it("fires hu-session-new when new chat clicked", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-session-new", () => {
      fired = true;
    });
    const btn = el.shadowRoot?.querySelector(".new-chat-btn") as HTMLElement;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });
});

describe("hu-file-preview", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-file-preview")).toBeDefined();
  });

  it("renders with no files", async () => {
    const el = document.createElement("hu-file-preview") as HTMLElement & {
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
    const el = document.createElement("hu-file-preview") as HTMLElement & {
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
    const el = document.createElement("hu-file-preview") as HTMLElement & {
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

  it("fires hu-file-remove when remove clicked", async () => {
    const el = document.createElement("hu-file-preview") as HTMLElement & {
      files: Array<{ name: string; size: number; type: string }>;
      updateComplete: Promise<boolean>;
    };
    el.files = [{ name: "a.txt", size: 100, type: "text/plain" }];
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { index: number } | null = null;
    el.addEventListener("hu-file-remove", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const removeBtn = el.shadowRoot?.querySelector(".remove-btn") as HTMLElement;
    removeBtn?.click();
    expect(detail).toEqual({ index: 0 });
    el.remove();
  });
});

describe("hu-context-menu", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-context-menu")).toBeDefined();
  });

  it("should default to closed", async () => {
    const el = document.createElement("hu-context-menu") as HTMLElement & {
      open: boolean;
    };
    expect(el.open).toBe(false);
  });

  it("positions at x,y when open", async () => {
    const el = document.createElement("hu-context-menu") as HTMLElement & {
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
    const el = document.createElement("hu-context-menu") as HTMLElement & {
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
    const el = document.createElement("hu-context-menu") as HTMLElement & {
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

describe("hu-stat-card", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-stat-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-stat-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render value and label", async () => {
    const el = document.createElement("hu-stat-card") as HTMLElement & {
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

  it("should render prefix when set with valueStr", async () => {
    const el = document.createElement("hu-stat-card") as HTMLElement & {
      valueStr: string;
      label: string;
      prefix: string;
      updateComplete: Promise<boolean>;
    };
    el.valueStr = "100";
    el.label = "Cost";
    el.prefix = "$";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("$");
    expect(el.shadowRoot?.textContent).toContain("100");
    el.remove();
  });

  it("should render suffix when set with valueStr", async () => {
    const el = document.createElement("hu-stat-card") as HTMLElement & {
      valueStr: string;
      label: string;
      suffix: string;
      updateComplete: Promise<boolean>;
    };
    el.valueStr = "5";
    el.label = "Duration";
    el.suffix = "s";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("5");
    expect(el.shadowRoot?.textContent).toContain("s");
    el.remove();
  });
});

describe("hu-stats-row", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-stats-row")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-stats-row");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render slot content", async () => {
    const el = document.createElement("hu-stats-row");
    const card = document.createElement("hu-stat-card");
    (card as HTMLElement & { value: number; label: string }).value = 42;
    (card as HTMLElement & { value: number; label: string }).label = "Tests";
    el.appendChild(card);
    document.body.appendChild(el);
    await (el as HTMLElement & { updateComplete: Promise<boolean> }).updateComplete;
    const slot = el.shadowRoot?.querySelector("slot");
    expect(slot).toBeDefined();
    el.remove();
  });
});

describe("hu-section-header", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-section-header")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-section-header");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render heading", async () => {
    const el = document.createElement("hu-section-header") as HTMLElement & {
      heading: string;
      updateComplete: Promise<boolean>;
    };
    el.heading = "Overview";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("Overview");
    el.remove();
  });

  it("should render description when set", async () => {
    const el = document.createElement("hu-section-header") as HTMLElement & {
      heading: string;
      description: string;
      updateComplete: Promise<boolean>;
    };
    el.heading = "Settings";
    el.description = "Configure your preferences";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("Settings");
    expect(el.shadowRoot?.textContent).toContain("Configure your preferences");
    el.remove();
  });
});

describe("hu-metric-row", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-metric-row")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-metric-row");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render items", async () => {
    const el = document.createElement("hu-metric-row") as HTMLElement & {
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

  it("should render multiple items from array", async () => {
    const el = document.createElement("hu-metric-row") as HTMLElement & {
      items: Array<{ label: string; value: string; accent?: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [
      { label: "A", value: "1" },
      { label: "B", value: "2" },
      { label: "C", value: "3", accent: "success" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("A");
    expect(el.shadowRoot?.textContent).toContain("B");
    expect(el.shadowRoot?.textContent).toContain("C");
    el.remove();
  });
});

describe("hu-timeline", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-timeline")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-timeline");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render items", async () => {
    const el = document.createElement("hu-timeline") as HTMLElement & {
      items: Array<{ time: string; message: string; status: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ time: "2m ago", message: "Test passed", status: "success" }];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("Test passed");
    el.remove();
  });

  it("should render multiple items from array", async () => {
    const el = document.createElement("hu-timeline") as HTMLElement & {
      items: Array<{ time: string; message: string; status: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [
      { time: "1m ago", message: "First", status: "success" },
      { time: "2m ago", message: "Second", status: "error" },
      { time: "3m ago", message: "Third", status: "info" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.textContent).toContain("First");
    expect(el.shadowRoot?.textContent).toContain("Second");
    expect(el.shadowRoot?.textContent).toContain("Third");
    el.remove();
  });

  it("should show empty state", async () => {
    const el = document.createElement("hu-timeline") as HTMLElement & {
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

describe("hu-sparkline-enhanced", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-sparkline-enhanced")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-sparkline-enhanced");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render SVG with data", async () => {
    const el = document.createElement("hu-sparkline-enhanced") as HTMLElement & {
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

describe("hu-forecast-chart", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-forecast-chart")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-forecast-chart");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render SVG with history data", async () => {
    const el = document.createElement("hu-forecast-chart") as HTMLElement & {
      history: Array<{ date: string; cost: number }>;
      projectedTotal: number;
      daysInMonth: number;
      updateComplete: Promise<boolean>;
    };
    el.history = [
      { date: "2026-03-01", cost: 1.83 },
      { date: "2026-03-02", cost: 2.14 },
      { date: "2026-03-03", cost: 3.07 },
    ];
    el.projectedTotal = 30;
    el.daysInMonth = 31;
    document.body.appendChild(el);
    await el.updateComplete;
    const svg = el.shadowRoot?.querySelector("svg");
    expect(svg).toBeTruthy();
    el.remove();
  });

  it("should not render with insufficient data", async () => {
    const el = document.createElement("hu-forecast-chart") as HTMLElement & {
      history: Array<{ date: string; cost: number }>;
      updateComplete: Promise<boolean>;
    };
    el.history = [{ date: "2026-03-01", cost: 1.0 }];
    document.body.appendChild(el);
    await el.updateComplete;
    const svg = el.shadowRoot?.querySelector("svg");
    expect(svg).toBeFalsy();
    el.remove();
  });
});

describe("hu-page-hero", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-page-hero")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-page-hero");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render slot content", async () => {
    const el = document.createElement("hu-page-hero") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    const p = document.createElement("p");
    p.textContent = "Hello";
    el.appendChild(p);
    document.body.appendChild(el);
    await el.updateComplete;
    const slot = el.shadowRoot?.querySelector("slot");
    expect(slot).toBeTruthy();
    expect(el.querySelector("p")?.textContent).toBe("Hello");
    el.remove();
  });
});

describe("hu-schedule-builder", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-schedule-builder")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-schedule-builder");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with shadow DOM", async () => {
    const el = document.createElement("hu-schedule-builder") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-message-actions", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-message-actions")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-message-actions");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept role and content properties", async () => {
    const el = document.createElement("hu-message-actions") as HTMLElement & {
      updateComplete: Promise<boolean>;
      role: string;
      content: string;
    };
    el.role = "assistant";
    el.content = "test content";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-automation-card", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-automation-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-automation-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render empty state without job", async () => {
    const el = document.createElement("hu-automation-card") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-automation-form", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-automation-form")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-automation-form");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render form fields", async () => {
    const el = document.createElement("hu-automation-form") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    el.setAttribute("type", "agent");
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-voice-orb", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-voice-orb")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-voice-orb");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render mic button and status", async () => {
    const el = document.createElement("hu-voice-orb") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    expect(el.shadowRoot?.querySelector(".mic-btn")).toBeTruthy();
    el.remove();
  });

  it("should dispatch hu-voice-mic-toggle on click", async () => {
    const el = document.createElement("hu-voice-orb") as HTMLElement & {
      disabled: boolean;
      updateComplete: Promise<boolean>;
    };
    el.disabled = false;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-voice-mic-toggle", () => {
      fired = true;
    });
    const btn = el.shadowRoot?.querySelector(".mic-btn") as HTMLButtonElement;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });
});

describe("hu-voice-conversation", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-voice-conversation")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-voice-conversation");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render empty state when no messages", async () => {
    const el = document.createElement("hu-voice-conversation") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    expect(el.shadowRoot?.querySelector("hu-empty-state")).toBeTruthy();
    el.remove();
  });

  it("has scrollToBottom method", async () => {
    const el = document.createElement("hu-voice-conversation") as HTMLElement & {
      updateComplete: Promise<boolean>;
      scrollToBottom: () => void;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(typeof el.scrollToBottom).toBe("function");
    expect(() => el.scrollToBottom()).not.toThrow();
    el.remove();
  });
});

describe("hu-chat-sessions-panel", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-chat-sessions-panel")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-chat-sessions-panel");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept open property", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as HTMLElement & {
      updateComplete: Promise<boolean>;
      open: boolean;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-file-preview", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-file-preview")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-file-preview");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render empty with no files", async () => {
    const el = document.createElement("hu-file-preview") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot).toBeTruthy();
    el.remove();
  });
});

describe("hu-chat-bubble", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-chat-bubble")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-chat-bubble");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default props", async () => {
    const el = document.createElement("hu-chat-bubble") as HTMLElement & {
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
    const el = document.createElement("hu-chat-bubble") as HTMLElement & {
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
    const el = document.createElement("hu-chat-bubble") as HTMLElement & {
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
    const el = document.createElement("hu-chat-bubble") as HTMLElement & {
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

describe("hu-typing-indicator", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-typing-indicator")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-typing-indicator");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default props", async () => {
    const el = document.createElement("hu-typing-indicator") as HTMLElement & {
      elapsed: string;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.elapsed).toBe("");
    el.remove();
  });

  it("renders elapsed time when provided", async () => {
    const el = document.createElement("hu-typing-indicator") as HTMLElement & {
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
    const el = document.createElement("hu-typing-indicator") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const indicator = el.shadowRoot?.querySelector(".indicator");
    expect(indicator?.getAttribute("role")).toBe("status");
    el.remove();
  });
});

describe("hu-delivery-status", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-delivery-status")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-delivery-status");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should render with default status", async () => {
    const el = document.createElement("hu-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.status).toBe("sent");
    el.remove();
  });

  it("status=failed shows retry button", async () => {
    const el = document.createElement("hu-delivery-status") as HTMLElement & {
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

  it("fires hu-retry when retry clicked", async () => {
    const el = document.createElement("hu-delivery-status") as HTMLElement & {
      status: string;
      updateComplete: Promise<boolean>;
    };
    el.status = "failed";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-retry", () => (fired = true));
    const retryBtn = el.shadowRoot?.querySelector(".retry") as HTMLElement | null;
    retryBtn?.click();
    expect(fired).toBe(true);
    el.remove();
  });

  it("has role=status for accessibility", async () => {
    const el = document.createElement("hu-delivery-status") as HTMLElement & {
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

describe("hu-message-group", () => {
  it("should be defined", () => {
    expect(customElements.get("hu-message-group")).toBeDefined();
  });
  it("defaults to assistant role", () => {
    const el = document.createElement("hu-message-group") as HTMLElement & { role: string };
    expect(el.role).toBe("assistant");
  });
  it("has role=group with aria-label", async () => {
    const el = document.createElement("hu-message-group") as HTMLElement & {
      role: string;
      updateComplete: Promise<boolean>;
    };
    el.role = "user";
    document.body.appendChild(el);
    await el.updateComplete;
    const g = el.shadowRoot?.querySelector("[role='group']");
    expect(g?.getAttribute("aria-label")).toBe("Your messages");
    el.remove();
  });
  it("has avatar and timestamp slots", async () => {
    const el = document.createElement("hu-message-group") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector('slot[name="avatar"]')).toBeTruthy();
    expect(el.shadowRoot?.querySelector('slot[name="timestamp"]')).toBeTruthy();
    el.remove();
  });
});

describe("hu-link-preview", () => {
  it("is defined as custom element", () => {
    expect(customElements.get("hu-link-preview")).toBeDefined();
  });
  it("renders nothing without url", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      updateComplete: Promise<boolean>;
    };
    el.url = "";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".card")).toBeNull();
    el.remove();
  });
  it("renders card with url and title", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      title: string;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://example.com";
    el.title = "Example";
    document.body.appendChild(el);
    await el.updateComplete;
    const card = el.shadowRoot?.querySelector("a.card") as HTMLAnchorElement;
    expect(card).toBeTruthy();
    expect(card?.target).toBe("_blank");
    expect(el.shadowRoot?.querySelector(".title")?.textContent).toBe("Example");
    el.remove();
  });
  it("extracts domain", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      title: string;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://www.github.com/repo";
    el.title = "Example";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".domain")?.textContent).toBe("github.com");
    el.remove();
  });
  it("uses title as aria-label", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      title: string;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://example.com";
    el.title = "My Title";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("a.card")?.getAttribute("aria-label")).toBe("My Title");
    el.remove();
  });
  it("shows skeleton loading state when loading=true", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      loading: boolean;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://example.com";
    el.loading = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const skeleton = el.shadowRoot?.querySelector(".skeleton-card");
    expect(skeleton).toBeTruthy();
    expect(el.shadowRoot?.querySelector("[aria-busy='true']")).toBeTruthy();
    el.remove();
  });
  it("shows fallback link when failed=true", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      failed: boolean;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://example.com/page";
    el.failed = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const fallback = el.shadowRoot?.querySelector(".fallback-link");
    expect(fallback).toBeTruthy();
    expect((fallback as HTMLAnchorElement)?.href).toContain("example.com");
    expect(fallback?.textContent).toContain("example.com");
    el.remove();
  });
  it("renders title and domain when provided", async () => {
    const el = document.createElement("hu-link-preview") as HTMLElement & {
      url: string;
      title: string;
      domain: string;
      updateComplete: Promise<boolean>;
    };
    el.url = "https://example.com";
    el.title = "Example Site";
    el.domain = "example.com";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".title")?.textContent).toBe("Example Site");
    expect(el.shadowRoot?.querySelector(".domain")?.textContent).toBe("example.com");
    el.remove();
  });
});

describe("hu-model-selector", () => {
  it("should be defined", () => {
    expect(customElements.get("hu-model-selector")).toBeDefined();
  });
  it("has combobox trigger", async () => {
    const el = document.createElement("hu-model-selector") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const t = el.shadowRoot?.querySelector("[role='combobox']");
    expect(t).toBeTruthy();
    expect(t?.getAttribute("aria-expanded")).toBe("false");
    el.remove();
  });
  it("opens dropdown on click", async () => {
    const el = document.createElement("hu-model-selector") as HTMLElement & {
      models: Array<{ id: string; name: string }>;
      updateComplete: Promise<boolean>;
    };
    el.models = [{ id: "a", name: "A" }];
    document.body.appendChild(el);
    await el.updateComplete;
    (el.shadowRoot?.querySelector("[role='combobox']") as HTMLElement)?.click();
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("[role='listbox']")).toBeTruthy();
    el.remove();
  });
  it("fires hu-model-change", async () => {
    const el = document.createElement("hu-model-selector") as HTMLElement & {
      models: Array<{ id: string; name: string }>;
      updateComplete: Promise<boolean>;
    };
    el.models = [
      { id: "a", name: "A" },
      { id: "b", name: "B" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    (el.shadowRoot?.querySelector("[role='combobox']") as HTMLElement)?.click();
    await el.updateComplete;
    let detail: { model: string } | null = null;
    el.addEventListener("hu-model-change", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    (el.shadowRoot?.querySelectorAll("[role='option']")?.[1] as HTMLElement)?.click();
    expect(detail).toEqual({ model: "b" });
    el.remove();
  });
  it("keyboard Escape closes", async () => {
    const el = document.createElement("hu-model-selector") as HTMLElement & {
      models: Array<{ id: string; name: string }>;
      updateComplete: Promise<boolean>;
    };
    el.models = [{ id: "a", name: "A" }];
    document.body.appendChild(el);
    await el.updateComplete;
    const t = el.shadowRoot?.querySelector("[role='combobox']") as HTMLElement;
    t?.click();
    await el.updateComplete;
    t?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    await el.updateComplete;
    expect(t?.getAttribute("aria-expanded")).toBe("false");
    el.remove();
  });
});

describe("hu-tapback-menu", () => {
  it("should be defined", () => {
    expect(customElements.get("hu-tapback-menu")).toBeDefined();
  });
  it("renders nothing when closed", async () => {
    const el = document.createElement("hu-tapback-menu") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = false;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".bar")).toBeNull();
    el.remove();
  });
  it("renders menu when open", async () => {
    const el = document.createElement("hu-tapback-menu") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("[role='menu']")).toBeTruthy();
    expect(el.shadowRoot?.querySelectorAll("[role='menuitem']")?.length).toBe(5);
    el.remove();
  });
  it("fires hu-react on click", async () => {
    const el = document.createElement("hu-tapback-menu") as HTMLElement & {
      open: boolean;
      messageIndex: number;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    el.messageIndex = 3;
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: Record<string, unknown> | null = null;
    el.addEventListener("hu-react", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    (el.shadowRoot?.querySelector("[role='menuitem']") as HTMLElement)?.click();
    expect(detail?.["index"]).toBe(3);
    el.remove();
  });
  it("fires hu-tapback-close on Escape", async () => {
    const el = document.createElement("hu-tapback-menu") as HTMLElement & {
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let closed = false;
    el.addEventListener("hu-tapback-close", () => (closed = true));
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    expect(closed).toBe(true);
    el.remove();
  });
});

describe("hu-chat-composer", () => {
  it("should be defined", () => {
    expect(customElements.get("hu-chat-composer")).toBeDefined();
  });
  it("has default props", () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      value: string;
      waiting: boolean;
      disabled: boolean;
      placeholder: string;
    };
    expect(el.value).toBe("");
    expect(el.waiting).toBe(false);
    expect(el.disabled).toBe(false);
    expect(el.placeholder).toBe("Type a message...");
  });
  it("renders textarea and send button", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("textarea")).toBeTruthy();
    expect(el.shadowRoot?.querySelector('[aria-label="Send"]')).toBeTruthy();
    el.remove();
  });
  it("send disabled when empty", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      value: string;
      updateComplete: Promise<boolean>;
    };
    el.value = "";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(
      (el.shadowRoot?.querySelector('[aria-label="Send"]') as HTMLButtonElement)?.disabled,
    ).toBe(true);
    el.remove();
  });
  it("shows stop button when waiting", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      waiting: boolean;
      updateComplete: Promise<boolean>;
    };
    el.waiting = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector('[aria-label="Stop generating"]')).toBeTruthy();
    expect(el.shadowRoot?.querySelector('[aria-label="Send"]')).toBeNull();
    el.remove();
  });
  it("fires hu-abort on stop click", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      waiting: boolean;
      updateComplete: Promise<boolean>;
    };
    el.waiting = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-abort", () => (fired = true));
    (el.shadowRoot?.querySelector('[aria-label="Stop generating"]') as HTMLElement)?.click();
    expect(fired).toBe(true);
    el.remove();
  });
  it("shows suggestions when enabled", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      showSuggestions: boolean;
      updateComplete: Promise<boolean>;
    };
    el.showSuggestions = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelectorAll(".pill")?.length).toBeGreaterThan(0);
    el.remove();
  });
  it("renders model chip", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      model: string;
      updateComplete: Promise<boolean>;
    };
    el.model = "GPT-4";
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".model-chip")?.textContent?.trim()).toBe("GPT-4");
    el.remove();
  });
  it("has attach file button", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector('[aria-label="Attach file"]')).toBeTruthy();
    el.remove();
  });
  it("shows mic button when voiceSupported", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector('[aria-label="Voice input"]')).toBeTruthy();
    el.remove();
  });
  it("hides mic button when voiceSupported is false", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      voiceSupported: boolean;
      updateComplete: Promise<boolean>;
    };
    el.voiceSupported = false;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector('[aria-label="Voice input"]')).toBeNull();
    el.remove();
  });
  it("dispatches hu-voice-start on mic click when not voiceActive", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      voiceActive: boolean;
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    let eventName = "";
    el.addEventListener("hu-voice-start", () => (eventName = "hu-voice-start"));
    (el.shadowRoot?.querySelector('[aria-label="Voice input"]') as HTMLElement)?.click();
    expect(eventName).toBe("hu-voice-start");
    el.remove();
  });
  it("dispatches hu-voice-stop on mic click when voiceActive", async () => {
    const el = document.createElement("hu-chat-composer") as HTMLElement & {
      voiceActive: boolean;
      updateComplete: Promise<boolean>;
    };
    el.voiceActive = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let eventName = "";
    el.addEventListener("hu-voice-stop", () => (eventName = "hu-voice-stop"));
    (el.shadowRoot?.querySelector('[aria-label="Voice input"]') as HTMLElement)?.click();
    expect(eventName).toBe("hu-voice-stop");
    el.remove();
  });
});

describe("hu-branch-tree", () => {
  it("is defined as custom element", () => {
    expect(customElements.get("hu-branch-tree")).toBeDefined();
  });
  it("has default props", () => {
    const el = document.createElement("hu-branch-tree") as HTMLElement & {
      branches: unknown[];
      activeId: string;
    };
    expect(el.branches).toEqual([]);
    expect(el.activeId).toBe("");
  });
  it("dispatches hu-branch-select on node click", async () => {
    const el = document.createElement("hu-branch-tree") as HTMLElement & {
      branches: Array<{
        id: string;
        label: string;
        children: unknown[];
        messagePreview: string;
        active: boolean;
      }>;
      updateComplete: Promise<boolean>;
    };
    el.branches = [
      { id: "a", label: "Branch A", children: [], messagePreview: "Preview", active: false },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    let receivedId: string | null = null;
    el.addEventListener("hu-branch-select", ((e: CustomEvent<{ id: string }>) => {
      receivedId = e.detail.id;
    }) as EventListener);
    const node = el.shadowRoot?.querySelector(".node-label");
    (node as HTMLElement)?.click();
    expect(receivedId).toBe("a");
    el.remove();
  });
  it("renders nodes from branches array", async () => {
    const el = document.createElement("hu-branch-tree") as HTMLElement & {
      branches: Array<{
        id: string;
        label: string;
        children: unknown[];
        messagePreview: string;
        active: boolean;
      }>;
      updateComplete: Promise<boolean>;
    };
    el.branches = [
      { id: "a", label: "Branch A", children: [], messagePreview: "Preview A", active: false },
      { id: "b", label: "Branch B", children: [], messagePreview: "Preview B", active: false },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    const nodes = el.shadowRoot?.querySelectorAll(".branch-node");
    expect(nodes?.length).toBe(2);
    expect(el.shadowRoot?.textContent).toContain("Branch A");
    expect(el.shadowRoot?.textContent).toContain("Branch B");
    el.remove();
  });
  it("marks active node with accent styling", async () => {
    const el = document.createElement("hu-branch-tree") as HTMLElement & {
      branches: Array<{
        id: string;
        label: string;
        children: unknown[];
        messagePreview: string;
        active: boolean;
      }>;
      activeId: string;
      updateComplete: Promise<boolean>;
    };
    el.branches = [
      { id: "a", label: "Branch A", children: [], messagePreview: "A", active: false },
      { id: "b", label: "Branch B", children: [], messagePreview: "B", active: false },
    ];
    el.activeId = "b";
    document.body.appendChild(el);
    await el.updateComplete;
    const activeNode = el.shadowRoot?.querySelector(".branch-node.active");
    expect(activeNode).toBeTruthy();
    expect(activeNode?.textContent).toContain("Branch B");
    el.remove();
  });
});

describe("hu-image-viewer", () => {
  it("is defined as custom element", () => {
    expect(customElements.get("hu-image-viewer")).toBeDefined();
  });

  it("renders backdrop when open", async () => {
    const el = document.createElement("hu-image-viewer") as HTMLElement & {
      src: string;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.src = "https://example.com/image.png";
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const backdrop = el.shadowRoot?.querySelector(".backdrop");
    expect(backdrop).toBeTruthy();
    el.remove();
  });

  it("dispatches close event on escape key", async () => {
    const el = document.createElement("hu-image-viewer") as HTMLElement & {
      src: string;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.src = "https://example.com/image.png";
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let closed = false;
    el.addEventListener("close", () => {
      closed = true;
    });
    const backdrop = el.shadowRoot?.querySelector(".backdrop");
    backdrop?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    expect(closed).toBe(true);
    el.remove();
  });

  it("hides when not open", async () => {
    const el = document.createElement("hu-image-viewer") as HTMLElement & {
      src: string;
      open: boolean;
      updateComplete: Promise<boolean>;
    };
    el.src = "https://example.com/image.png";
    el.open = false;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector(".backdrop")).toBeNull();
    el.remove();
  });
});

describe("hu-message-thread", () => {
  it("should be defined", () => {
    expect(customElements.get("hu-message-thread")).toBeDefined();
  });
  it("has default props", () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: unknown[];
      isWaiting: boolean;
      historyLoading: boolean;
    };
    expect(el.items).toEqual([]);
    expect(el.isWaiting).toBe(false);
    expect(el.historyLoading).toBe(false);
  });
  it("renders role=log container", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const c = el.shadowRoot?.querySelector("[role='log']");
    expect(c).toBeTruthy();
    expect(c?.getAttribute("aria-live")).toBe("polite");
    el.remove();
  });
  it("renders messages as hu-chat-bubble", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: Array<{ type: string; role: string; content: string; ts: number }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [
      { type: "message", role: "user", content: "Hello", ts: Date.now() },
      { type: "message", role: "assistant", content: "Hi", ts: Date.now() },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelectorAll("hu-chat-bubble")?.length).toBe(2);
    el.remove();
  });
  it("groups consecutive same-role messages", async () => {
    const now = Date.now();
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: Array<{ type: string; role: string; content: string; ts: number }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [
      { type: "message", role: "user", content: "a", ts: now },
      { type: "message", role: "user", content: "b", ts: now + 1000 },
      { type: "message", role: "assistant", content: "c", ts: now + 2000 },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelectorAll("hu-message-group")?.length).toBe(2);
    el.remove();
  });
  it("shows typing indicator when waiting", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      isWaiting: boolean;
      items: Array<{ type: string; role: string; content: string; ts: number }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ type: "message", role: "user", content: "hello", ts: Date.now() }];
    el.isWaiting = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("hu-typing-indicator")).toBeTruthy();
    el.remove();
  });
  it("shows skeleton when historyLoading", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      historyLoading: boolean;
      updateComplete: Promise<boolean>;
    };
    el.historyLoading = true;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("hu-skeleton")).toBeTruthy();
    el.remove();
  });
  it("renders tool calls", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: Array<{ type: string; name?: string; status?: string; result?: string }>;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ type: "tool_call", name: "shell", status: "completed", result: "ok" }];
    document.body.appendChild(el);
    await el.updateComplete;
    expect(el.shadowRoot?.querySelector("hu-tool-result")).toBeTruthy();
    el.remove();
  });
  it("has scrollToBottom method", () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      scrollToBottom: () => void;
    };
    expect(typeof el.scrollToBottom).toBe("function");
  });
  it("should render load-earlier button when hasEarlierMessages is true", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: Array<{ type: string; role: string; content: string; ts: number }>;
      hasEarlierMessages: boolean;
      loadingEarlier: boolean;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ type: "message", role: "user", content: "test", ts: Date.now() }];
    el.hasEarlierMessages = true;
    el.loadingEarlier = false;
    document.body.appendChild(el);
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector(".load-earlier-btn");
    expect(btn).toBeTruthy();
    expect(btn?.textContent).toContain("Load earlier messages");
    el.remove();
  });
  it("should fire hu-load-earlier event on button click", async () => {
    const el = document.createElement("hu-message-thread") as HTMLElement & {
      items: Array<{ type: string; role: string; content: string; ts: number }>;
      hasEarlierMessages: boolean;
      loadingEarlier: boolean;
      updateComplete: Promise<boolean>;
    };
    el.items = [{ type: "message", role: "user", content: "test", ts: Date.now() }];
    el.hasEarlierMessages = true;
    el.loadingEarlier = false;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-load-earlier", () => (fired = true));
    const btn = el.shadowRoot?.querySelector(".load-earlier-btn") as HTMLElement;
    btn?.click();
    expect(fired).toBe(true);
    el.remove();
  });
});

describe("hu-chart", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-chart")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-chart");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept type property", () => {
    const el = document.createElement("hu-chart") as ScChart;
    el.type = "bar";
    expect(el.type).toBe("bar");
  });

  it("should accept data property", () => {
    const el = document.createElement("hu-chart") as ScChart;
    const data = { labels: ["A", "B"], datasets: [{ data: [1, 2] }] };
    el.data = data;
    expect(el.data).toEqual(data);
  });

  it("should default height to 200", () => {
    const el = document.createElement("hu-chart") as ScChart;
    expect(el.height).toBe(200);
  });

  it("should default horizontal to false", () => {
    const el = document.createElement("hu-chart") as ScChart;
    expect(el.horizontal).toBe(false);
  });

  it("should accept horizontal property", () => {
    const el = document.createElement("hu-chart") as ScChart;
    el.horizontal = true;
    expect(el.horizontal).toBe(true);
  });

  it("should render a canvas when data has entries", async () => {
    const el = document.createElement("hu-chart") as ScChart;
    el.type = "bar";
    el.data = { labels: ["A"], datasets: [{ data: [1] }] };
    document.body.appendChild(el);
    await el.updateComplete;
    const canvas = el.shadowRoot?.querySelector("canvas");
    expect(canvas).toBeTruthy();
    el.remove();
  });

  it("should show empty message when no data", async () => {
    const el = document.createElement("hu-chart") as ScChart;
    el.data = { labels: [], datasets: [] };
    document.body.appendChild(el);
    await el.updateComplete;
    const empty = el.shadowRoot?.querySelector(".empty");
    expect(empty).toBeTruthy();
    expect(empty?.textContent).toContain("No data");
    el.remove();
  });
});

describe("hu-activity-timeline", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-activity-timeline")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-activity-timeline");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-overview-stats", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-overview-stats")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-overview-stats");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-sessions-table", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-sessions-table")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-sessions-table");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-skill-card", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-skill-card")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-skill-card");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-skill-detail", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-skill-detail")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-skill-detail");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("hu-skill-registry", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-skill-registry")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-skill-registry");
    expect(el).toBeInstanceOf(HTMLElement);
  });
});

describe("spring animation library", () => {
  it("SPRING_PRESETS has expected keys", () => {
    const expected = ["micro", "standard", "expressive", "dramatic", "gentle", "bounce", "snappy"];
    for (const key of expected) {
      expect(SPRING_PRESETS).toHaveProperty(key);
      expect(SPRING_PRESETS[key as keyof typeof SPRING_PRESETS]).toHaveProperty("stiffness");
      expect(SPRING_PRESETS[key as keyof typeof SPRING_PRESETS]).toHaveProperty("damping");
      expect(SPRING_PRESETS[key as keyof typeof SPRING_PRESETS]).toHaveProperty("mass");
    }
  });

  it("springAnimate returns stop function and promise", () => {
    const el = document.createElement("div");
    document.body.appendChild(el);
    const result = springAnimate(
      el,
      [{ property: "opacity", from: 0, to: 1 }],
      SPRING_PRESETS.standard,
    );
    expect(typeof result.stop).toBe("function");
    expect(result.promise).toBeInstanceOf(Promise);
    result.stop();
    el.remove();
  });

  it("respects prefers-reduced-motion", async () => {
    const matchMediaSpy = vi.spyOn(window, "matchMedia").mockImplementation((query: string) => {
      if (query === "(prefers-reduced-motion: reduce)") {
        return { matches: true, addListener: vi.fn(), removeListener: vi.fn() } as MediaQueryList;
      }
      return window.matchMedia(query);
    });
    try {
      const el = document.createElement("div");
      document.body.appendChild(el);
      const result = springAnimate(
        el,
        [{ property: "opacity", from: 0, to: 1 }],
        SPRING_PRESETS.standard,
      );
      expect(el.style.opacity).toBe("1");
      expect(typeof result.stop).toBe("function");
      await result.promise;
      el.remove();
    } finally {
      matchMediaSpy.mockRestore();
    }
  });
});
