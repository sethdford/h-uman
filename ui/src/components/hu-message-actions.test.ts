import { describe, it, expect, vi } from "vitest";
import "./hu-message-actions.js";

type ActionsEl = HTMLElement & {
  content: string;
  role: string;
  updateComplete: Promise<boolean>;
};

describe("hu-message-actions", () => {
  it("registers as custom element", () => {
    expect(customElements.get("hu-message-actions")).toBeDefined();
  });

  it("renders copy button", async () => {
    const el = document.createElement("hu-message-actions") as ActionsEl;
    el.content = "Hello world";
    document.body.appendChild(el);
    await el.updateComplete;
    const copyBtn = el.shadowRoot?.querySelector('[aria-label="Copy"]');
    expect(copyBtn).toBeTruthy();
    el.remove();
  });

  it("renders regenerate button for assistant messages", async () => {
    const el = document.createElement("hu-message-actions") as ActionsEl;
    el.content = "Response";
    el.role = "assistant";
    document.body.appendChild(el);
    await el.updateComplete;
    const regenBtn = el.shadowRoot?.querySelector('[aria-label="Regenerate"]');
    expect(regenBtn).toBeTruthy();
    el.remove();
  });

  it("fires hu-copy event on copy click", async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(navigator, "clipboard", {
      value: { writeText },
      configurable: true,
    });
    const onCopy = vi.fn();
    const el = document.createElement("hu-message-actions") as ActionsEl;
    el.content = "Test content";
    el.addEventListener("hu-copy", onCopy);
    document.body.appendChild(el);
    await el.updateComplete;
    const copyBtn = el.shadowRoot?.querySelector('[aria-label="Copy"]') as HTMLButtonElement;
    copyBtn?.click();
    await vi.waitFor(() => expect(onCopy).toHaveBeenCalledTimes(1));
    expect(writeText).toHaveBeenCalledWith("Test content");
    el.remove();
  });
});
