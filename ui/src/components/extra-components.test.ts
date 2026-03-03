import { describe, it, expect } from "vitest";

import "./floating-mic.js";
import "./sidebar.js";
import "./command-palette.js";

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
