import { describe, it, expect } from "vitest";

describe("hu-button", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-button.js");
    expect(customElements.get("hu-button")).toBeDefined();
  });

  it("should reflect variant property", async () => {
    const { ScButton } = await import("./hu-button.js");
    const el = new ScButton();
    el.variant = "primary";
    expect(el.variant).toBe("primary");
  });

  it("should reflect disabled property", async () => {
    const { ScButton } = await import("./hu-button.js");
    const el = new ScButton();
    el.disabled = true;
    expect(el.disabled).toBe(true);
  });
});

describe("hu-card", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-card.js");
    expect(customElements.get("hu-card")).toBeDefined();
  });
});

describe("hu-badge", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-badge.js");
    expect(customElements.get("hu-badge")).toBeDefined();
  });

  it("should reflect variant property", async () => {
    const { ScBadge } = await import("./hu-badge.js");
    const el = new ScBadge();
    el.variant = "success";
    expect(el.variant).toBe("success");
  });
});

describe("hu-modal", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-modal.js");
    expect(customElements.get("hu-modal")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScModal } = await import("./hu-modal.js");
    const el = new ScModal();
    expect(el.open).toBe(false);
  });
});

describe("hu-tooltip", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-tooltip.js");
    expect(customElements.get("hu-tooltip")).toBeDefined();
  });
});

describe("hu-empty-state", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-empty-state.js");
    expect(customElements.get("hu-empty-state")).toBeDefined();
  });
});

describe("hu-skeleton", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-skeleton.js");
    expect(customElements.get("hu-skeleton")).toBeDefined();
  });
});

describe("hu-sheet", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-sheet.js");
    expect(customElements.get("hu-sheet")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScSheet } = await import("./hu-sheet.js");
    const el = new ScSheet();
    expect(el.open).toBe(false);
  });
});

describe("hu-toast", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-toast.js");
    expect(customElements.get("hu-toast")).toBeDefined();
  });
});

describe("hu-tabs", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-tabs.js");
    expect(customElements.get("hu-tabs")).toBeDefined();
  });

  it("should accept tabs array", async () => {
    const { ScTabs } = await import("./hu-tabs.js");
    const el = new ScTabs();
    el.tabs = [
      { id: "a", label: "A" },
      { id: "b", label: "B" },
    ];
    expect(el.tabs).toHaveLength(2);
  });
});

describe("hu-avatar", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-avatar.js");
    expect(customElements.get("hu-avatar")).toBeDefined();
  });

  it("should store name property", async () => {
    const { ScAvatar } = await import("./hu-avatar.js");
    const el = new ScAvatar();
    el.name = "John Doe";
    expect(el.name).toBe("John Doe");
  });
});

describe("hu-status-dot", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-status-dot.js");
    expect(customElements.get("hu-status-dot")).toBeDefined();
  });

  it("should default to disconnected status and sm size", async () => {
    const { ScStatusDot } = await import("./hu-status-dot.js");
    const el = new ScStatusDot();
    expect(el.status).toBe("disconnected");
    expect(el.size).toBe("sm");
  });

  it("should reflect status and size properties", async () => {
    const { ScStatusDot } = await import("./hu-status-dot.js");
    const el = new ScStatusDot();
    el.status = "connected";
    el.size = "md";
    expect(el.status).toBe("connected");
    expect(el.size).toBe("md");
  });
});

describe("hu-progress", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-progress.js");
    expect(customElements.get("hu-progress")).toBeDefined();
  });

  it("should default to 0 value", async () => {
    const { ScProgress } = await import("./hu-progress.js");
    const el = new ScProgress();
    expect(el.value).toBe(0);
  });

  it("should accept indeterminate mode", async () => {
    const { ScProgress } = await import("./hu-progress.js");
    const el = new ScProgress();
    el.indeterminate = true;
    expect(el.indeterminate).toBe(true);
  });
});

describe("hu-input", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-input.js");
    expect(customElements.get("hu-input")).toBeDefined();
  });

  it("should reflect value and default properties", async () => {
    const { ScInput } = await import("./hu-input.js");
    const el = new ScInput();
    expect(el.value).toBe("");
    expect(el.type).toBe("text");
    expect(el.size).toBe("md");
  });

  it("should fire hu-input event on input", async () => {
    const { ScInput } = await import("./hu-input.js");
    const el = new ScInput();
    document.body.appendChild(el);
    await el.updateComplete;
    const input = el.shadowRoot?.querySelector("input") as HTMLInputElement;
    let fired = false;
    el.addEventListener("hu-input", () => {
      fired = true;
    });
    input.value = "test";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    await el.updateComplete;
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });
});

describe("hu-select", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-select.js");
    expect(customElements.get("hu-select")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScSelect } = await import("./hu-select.js");
    const el = new ScSelect();
    expect(el.value).toBe("");
    expect(el.size).toBe("md");
    expect(el.options).toEqual([]);
  });

  it("should accept options array", async () => {
    const { ScSelect } = await import("./hu-select.js");
    const el = new ScSelect();
    el.options = [
      { value: "a", label: "Option A" },
      { value: "b", label: "Option B" },
    ];
    expect(el.options).toHaveLength(2);
  });
});

describe("hu-dropdown", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-dropdown.js");
    expect(customElements.get("hu-dropdown")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScDropdown } = await import("./hu-dropdown.js");
    const el = new ScDropdown();
    expect(el.open).toBe(false);
  });

  it("should reflect align property", async () => {
    const { ScDropdown } = await import("./hu-dropdown.js");
    const el = new ScDropdown();
    el.align = "end";
    expect(el.align).toBe("end");
  });
});

describe("hu-switch", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-switch.js");
    expect(customElements.get("hu-switch")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScSwitch } = await import("./hu-switch.js");
    const el = new ScSwitch();
    expect(el.checked).toBe(false);
    expect(el.disabled).toBe(false);
    expect(el.label).toBe("");
  });

  it("should fire hu-change with checked when toggled", async () => {
    const { ScSwitch } = await import("./hu-switch.js");
    const el = new ScSwitch();
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { checked: boolean } | null = null;
    el.addEventListener("hu-change", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const switchEl = el.shadowRoot?.querySelector("[role='switch']") as HTMLElement;
    switchEl?.click();
    await el.updateComplete;
    expect(detail).toEqual({ checked: true });
    document.body.removeChild(el);
  });
});

describe("hu-radio", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-radio.js");
    expect(customElements.get("hu-radio")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScRadio } = await import("./hu-radio.js");
    const el = new ScRadio();
    expect(el.value).toBe("");
    expect(el.options).toEqual([]);
    expect(el.name).toBe("");
    expect(el.disabled).toBe(false);
  });

  it("should fire hu-change when option selected", async () => {
    const { ScRadio } = await import("./hu-radio.js");
    const el = new ScRadio();
    el.options = [
      { value: "a", label: "Option A" },
      { value: "b", label: "Option B" },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { value: string } | null = null;
    el.addEventListener("hu-change", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const input = el.shadowRoot?.querySelector('input[value="b"]') as HTMLInputElement;
    input?.click();
    await el.updateComplete;
    expect(detail).toEqual({ value: "b" });
    document.body.removeChild(el);
  });
});

describe("hu-textarea", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-textarea.js");
    expect(customElements.get("hu-textarea")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScTextarea } = await import("./hu-textarea.js");
    const el = new ScTextarea();
    expect(el.value).toBe("");
    expect(el.rows).toBe(4);
    expect(el.resize).toBe("vertical");
  });
});

describe("hu-popover", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-popover.js");
    expect(customElements.get("hu-popover")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScPopover } = await import("./hu-popover.js");
    const el = new ScPopover();
    expect(el.open).toBe(false);
    expect(el.position).toBe("bottom");
    expect(el.arrow).toBe(true);
  });
});

describe("hu-breadcrumb", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-breadcrumb.js");
    expect(customElements.get("hu-breadcrumb")).toBeDefined();
  });

  it("should accept items array", async () => {
    const { ScBreadcrumb } = await import("./hu-breadcrumb.js");
    const el = new ScBreadcrumb();
    el.items = [
      { label: "Home", href: "/" },
      { label: "Settings", href: "/settings" },
      { label: "Profile" },
    ];
    expect(el.items).toHaveLength(3);
  });
});

describe("hu-dialog", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-dialog.js");
    expect(customElements.get("hu-dialog")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScDialog } = await import("./hu-dialog.js");
    const el = new ScDialog();
    expect(el.open).toBe(false);
  });

  it("should fire hu-confirm on confirm button click", async () => {
    const { ScDialog } = await import("./hu-dialog.js");
    const el = new ScDialog();
    el.open = true;
    el.title = "Test";
    el.message = "Test message";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-confirm", () => {
      fired = true;
    });
    const confirmBtn = el.shadowRoot?.querySelector(".btn-confirm-default, .btn-confirm-danger");
    (confirmBtn as HTMLElement)?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });

  it("should fire hu-cancel on cancel button click", async () => {
    const { ScDialog } = await import("./hu-dialog.js");
    const el = new ScDialog();
    el.open = true;
    el.title = "Test";
    el.message = "Test message";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-cancel", () => {
      fired = true;
    });
    const cancelBtn = el.shadowRoot?.querySelector(".btn-cancel");
    (cancelBtn as HTMLElement)?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });
});

describe("hu-slider", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-slider.js");
    expect(customElements.get("hu-slider")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScSlider } = await import("./hu-slider.js");
    const el = new ScSlider();
    expect(el.value).toBe(50);
    expect(el.min).toBe(0);
    expect(el.max).toBe(100);
    expect(el.step).toBe(1);
    expect(el.showValue).toBe(true);
  });

  it("should have ARIA attributes", async () => {
    const { ScSlider } = await import("./hu-slider.js");
    const el = new ScSlider();
    el.label = "Volume";
    document.body.appendChild(el);
    await el.updateComplete;
    const input = el.shadowRoot?.querySelector("input[type='range']");
    expect(input?.getAttribute("role")).toBe("slider");
    expect(input?.getAttribute("aria-valuemin")).toBe("0");
    expect(input?.getAttribute("aria-valuemax")).toBe("100");
    expect(input?.getAttribute("aria-valuenow")).toBe("50");
    document.body.removeChild(el);
  });

  it("should fire hu-change when value changes", async () => {
    const { ScSlider } = await import("./hu-slider.js");
    const el = new ScSlider();
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { value: number } | undefined;
    el.addEventListener("hu-change", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const input = el.shadowRoot?.querySelector("input") as HTMLInputElement;
    input.value = "75";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    await el.updateComplete;
    expect(detail!.value).toBe(75);
    document.body.removeChild(el);
  });
});

describe("hu-search", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-search.js");
    expect(customElements.get("hu-search")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScSearch } = await import("./hu-search.js");
    const el = new ScSearch();
    expect(el.value).toBe("");
    expect(el.placeholder).toBe("Search...");
    expect(el.size).toBe("md");
  });

  it("should have role search", async () => {
    const { ScSearch } = await import("./hu-search.js");
    const el = new ScSearch();
    document.body.appendChild(el);
    await el.updateComplete;
    const wrapper = el.shadowRoot?.querySelector(".wrapper");
    expect(wrapper?.getAttribute("role")).toBe("search");
    document.body.removeChild(el);
  });
});

describe("hu-segmented-control", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-segmented-control.js");
    expect(customElements.get("hu-segmented-control")).toBeDefined();
  });

  it("should reflect options and value", async () => {
    const { ScSegmentedControl } = await import("./hu-segmented-control.js");
    const el = new ScSegmentedControl();
    el.options = [
      { value: "a", label: "Option A" },
      { value: "b", label: "Option B" },
    ];
    el.value = "b";
    expect(el.options).toHaveLength(2);
    expect(el.value).toBe("b");
  });

  it("should have role tablist and tab", async () => {
    const { ScSegmentedControl } = await import("./hu-segmented-control.js");
    const el = new ScSegmentedControl();
    el.options = [
      { value: "a", label: "A" },
      { value: "b", label: "B" },
    ];
    el.value = "a";
    document.body.appendChild(el);
    await el.updateComplete;
    const container = el.shadowRoot?.querySelector(".container");
    expect(container?.getAttribute("role")).toBe("tablist");
    const tabs = el.shadowRoot?.querySelectorAll("[role='tab']");
    expect(tabs?.length).toBe(2);
    const activeTab = el.shadowRoot?.querySelector(".segment.active");
    expect(activeTab?.getAttribute("aria-selected")).toBe("true");
    document.body.removeChild(el);
  });
});

describe("hu-chat-search", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-chat-search.js");
    expect(customElements.get("hu-chat-search")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    expect(el.open).toBe(false);
    expect(el.query).toBe("");
    expect(el.matchCount).toBe(0);
    expect(el.currentMatch).toBe(0);
  });

  it("should render when open", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const bar = el.shadowRoot?.querySelector(".bar");
    const input = el.shadowRoot?.querySelector("#search-input");
    expect(bar).toBeTruthy();
    expect(input).toBeTruthy();
    document.body.removeChild(el);
  });

  it("should have search input when opened", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const input = el.shadowRoot?.querySelector("#search-input") as HTMLInputElement;
    expect(input).toBeTruthy();
    expect(input?.type).toBe("search");
    expect(input?.getAttribute("aria-label")).toBe("Search query");
    document.body.removeChild(el);
  });

  it("should fire hu-search-change when query changes", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let detail: { query: string } | null = null;
    el.addEventListener("hu-search-change", ((e: CustomEvent) => {
      detail = e.detail;
    }) as EventListener);
    const input = el.shadowRoot?.querySelector("#search-input") as HTMLInputElement;
    input.value = "test";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    await el.updateComplete;
    expect(detail).toEqual({ query: "test" });
    document.body.removeChild(el);
  });

  it("should fire hu-search-close when close button clicked", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-search-close", () => {
      fired = true;
    });
    const closeBtn = el.shadowRoot?.querySelector(".close-btn") as HTMLElement;
    closeBtn?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });

  it("should fire hu-search-next when next button clicked", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    el.matchCount = 2;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-search-next", () => {
      fired = true;
    });
    const nextBtn = el.shadowRoot?.querySelectorAll(".nav-btn")[1] as HTMLElement;
    nextBtn?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });

  it("should fire hu-search-prev when prev button clicked", async () => {
    const { ScChatSearch } = await import("./hu-chat-search.js");
    const el = new ScChatSearch();
    el.open = true;
    el.matchCount = 2;
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("hu-search-prev", () => {
      fired = true;
    });
    const prevBtn = el.shadowRoot?.querySelectorAll(".nav-btn")[0] as HTMLElement;
    prevBtn?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });
});

describe("hu-date-picker", () => {
  it("should be defined as a custom element", async () => {
    await import("./hu-date-picker.js");
    expect(customElements.get("hu-date-picker")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScDatePicker } = await import("./hu-date-picker.js");
    const el = new ScDatePicker();
    expect(el.value).toBe("");
    expect(el.label).toBe("");
    expect(el.disabled).toBe(false);
  });

  it("should have label associated with input", async () => {
    const { ScDatePicker } = await import("./hu-date-picker.js");
    const el = new ScDatePicker();
    el.label = "Date";
    document.body.appendChild(el);
    await el.updateComplete;
    const label = el.shadowRoot?.querySelector("label");
    const input = el.shadowRoot?.querySelector("input");
    expect(label?.getAttribute("for")).toBe(input?.id);
    document.body.removeChild(el);
  });
});
