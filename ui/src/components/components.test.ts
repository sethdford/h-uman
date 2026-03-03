import { describe, it, expect } from "vitest";

describe("sc-button", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-button.js");
    expect(customElements.get("sc-button")).toBeDefined();
  });

  it("should reflect variant property", async () => {
    const { ScButton } = await import("./sc-button.js");
    const el = new ScButton();
    el.variant = "primary";
    expect(el.variant).toBe("primary");
  });

  it("should reflect disabled property", async () => {
    const { ScButton } = await import("./sc-button.js");
    const el = new ScButton();
    el.disabled = true;
    expect(el.disabled).toBe(true);
  });
});

describe("sc-card", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-card.js");
    expect(customElements.get("sc-card")).toBeDefined();
  });
});

describe("sc-badge", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-badge.js");
    expect(customElements.get("sc-badge")).toBeDefined();
  });

  it("should reflect variant property", async () => {
    const { ScBadge } = await import("./sc-badge.js");
    const el = new ScBadge();
    el.variant = "success";
    expect(el.variant).toBe("success");
  });
});

describe("sc-modal", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-modal.js");
    expect(customElements.get("sc-modal")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScModal } = await import("./sc-modal.js");
    const el = new ScModal();
    expect(el.open).toBe(false);
  });
});

describe("sc-tooltip", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-tooltip.js");
    expect(customElements.get("sc-tooltip")).toBeDefined();
  });
});

describe("sc-empty-state", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-empty-state.js");
    expect(customElements.get("sc-empty-state")).toBeDefined();
  });
});

describe("sc-skeleton", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-skeleton.js");
    expect(customElements.get("sc-skeleton")).toBeDefined();
  });
});

describe("sc-sheet", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-sheet.js");
    expect(customElements.get("sc-sheet")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScSheet } = await import("./sc-sheet.js");
    const el = new ScSheet();
    expect(el.open).toBe(false);
  });
});

describe("sc-toast", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-toast.js");
    expect(customElements.get("sc-toast")).toBeDefined();
  });
});

describe("sc-tabs", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-tabs.js");
    expect(customElements.get("sc-tabs")).toBeDefined();
  });

  it("should accept tabs array", async () => {
    const { ScTabs } = await import("./sc-tabs.js");
    const el = new ScTabs();
    el.tabs = [
      { id: "a", label: "A" },
      { id: "b", label: "B" },
    ];
    expect(el.tabs).toHaveLength(2);
  });
});

describe("sc-avatar", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-avatar.js");
    expect(customElements.get("sc-avatar")).toBeDefined();
  });

  it("should store name property", async () => {
    const { ScAvatar } = await import("./sc-avatar.js");
    const el = new ScAvatar();
    el.name = "John Doe";
    expect(el.name).toBe("John Doe");
  });
});

describe("sc-progress", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-progress.js");
    expect(customElements.get("sc-progress")).toBeDefined();
  });

  it("should default to 0 value", async () => {
    const { ScProgress } = await import("./sc-progress.js");
    const el = new ScProgress();
    expect(el.value).toBe(0);
  });

  it("should accept indeterminate mode", async () => {
    const { ScProgress } = await import("./sc-progress.js");
    const el = new ScProgress();
    el.indeterminate = true;
    expect(el.indeterminate).toBe(true);
  });
});

describe("sc-input", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-input.js");
    expect(customElements.get("sc-input")).toBeDefined();
  });

  it("should reflect value and default properties", async () => {
    const { ScInput } = await import("./sc-input.js");
    const el = new ScInput();
    expect(el.value).toBe("");
    expect(el.type).toBe("text");
    expect(el.size).toBe("md");
  });

  it("should fire sc-input event on input", async () => {
    const { ScInput } = await import("./sc-input.js");
    const el = new ScInput();
    document.body.appendChild(el);
    await el.updateComplete;
    const input = el.shadowRoot?.querySelector("input") as HTMLInputElement;
    let fired = false;
    el.addEventListener("sc-input", () => {
      fired = true;
    });
    input.value = "test";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    await el.updateComplete;
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });
});

describe("sc-select", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-select.js");
    expect(customElements.get("sc-select")).toBeDefined();
  });

  it("should reflect default properties", async () => {
    const { ScSelect } = await import("./sc-select.js");
    const el = new ScSelect();
    expect(el.value).toBe("");
    expect(el.size).toBe("md");
    expect(el.options).toEqual([]);
  });

  it("should accept options array", async () => {
    const { ScSelect } = await import("./sc-select.js");
    const el = new ScSelect();
    el.options = [
      { value: "a", label: "Option A" },
      { value: "b", label: "Option B" },
    ];
    expect(el.options).toHaveLength(2);
  });
});

describe("sc-dropdown", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-dropdown.js");
    expect(customElements.get("sc-dropdown")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScDropdown } = await import("./sc-dropdown.js");
    const el = new ScDropdown();
    expect(el.open).toBe(false);
  });

  it("should reflect align property", async () => {
    const { ScDropdown } = await import("./sc-dropdown.js");
    const el = new ScDropdown();
    el.align = "end";
    expect(el.align).toBe("end");
  });
});

describe("sc-dialog", () => {
  it("should be defined as a custom element", async () => {
    await import("./sc-dialog.js");
    expect(customElements.get("sc-dialog")).toBeDefined();
  });

  it("should default to closed", async () => {
    const { ScDialog } = await import("./sc-dialog.js");
    const el = new ScDialog();
    expect(el.open).toBe(false);
  });

  it("should fire sc-confirm on confirm button click", async () => {
    const { ScDialog } = await import("./sc-dialog.js");
    const el = new ScDialog();
    el.open = true;
    el.title = "Test";
    el.message = "Test message";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("sc-confirm", () => {
      fired = true;
    });
    const confirmBtn = el.shadowRoot?.querySelector(".btn-confirm-default, .btn-confirm-danger");
    (confirmBtn as HTMLElement)?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });

  it("should fire sc-cancel on cancel button click", async () => {
    const { ScDialog } = await import("./sc-dialog.js");
    const el = new ScDialog();
    el.open = true;
    el.title = "Test";
    el.message = "Test message";
    document.body.appendChild(el);
    await el.updateComplete;
    let fired = false;
    el.addEventListener("sc-cancel", () => {
      fired = true;
    });
    const cancelBtn = el.shadowRoot?.querySelector(".btn-cancel");
    (cancelBtn as HTMLElement)?.click();
    expect(fired).toBe(true);
    document.body.removeChild(el);
  });
});
