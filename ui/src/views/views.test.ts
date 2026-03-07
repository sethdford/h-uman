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
import "./cron-view.js";
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
  "sc-cron-view",
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

  it("has suggested prompt pills in composer when empty", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const composer = el.shadowRoot?.querySelector("sc-composer");
    const pills = composer?.shadowRoot?.querySelectorAll(".prompt-pill") ?? [];
    expect(pills.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("message list has role=log and aria-live", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const messageList = el.shadowRoot?.querySelector("sc-message-list");
    const scrollContainer = messageList?.shadowRoot?.querySelector("#scroll-container");
    expect(scrollContainer?.getAttribute("role")).toBe("log");
    expect(scrollContainer?.getAttribute("aria-live")).toBe("polite");
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
