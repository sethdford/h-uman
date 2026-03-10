import { describe, it, expect, vi } from "vitest";
import "./hu-file-preview.js";

type FilePreviewEl = HTMLElement & {
  files: Array<{ name: string; type: string; size: number; dataUrl?: string }>;
  updateComplete: Promise<boolean>;
};

describe("hu-file-preview", () => {
  it("registers as custom element", () => {
    expect(customElements.get("hu-file-preview")).toBeDefined();
  });

  it("renders file cards", async () => {
    const el = document.createElement("hu-file-preview") as FilePreviewEl;
    el.files = [
      { name: "test.txt", type: "text/plain", size: 1024 },
      {
        name: "image.png",
        type: "image/png",
        size: 2048,
        dataUrl: "data:image/png;base64,abc",
      },
    ];
    document.body.appendChild(el);
    await el.updateComplete;
    const cards = el.shadowRoot?.querySelectorAll(".card") ?? [];
    expect(cards.length).toBe(2);
    el.remove();
  });

  it("fires hu-file-remove on remove click", async () => {
    const onRemove = vi.fn();
    const el = document.createElement("hu-file-preview") as FilePreviewEl;
    el.files = [{ name: "test.txt", type: "text/plain", size: 1024 }];
    el.addEventListener("hu-file-remove", onRemove);
    document.body.appendChild(el);
    await el.updateComplete;
    const removeBtn = el.shadowRoot?.querySelector(".remove-btn") as HTMLButtonElement;
    removeBtn?.click();
    expect(onRemove).toHaveBeenCalledTimes(1);
    el.remove();
  });
});
