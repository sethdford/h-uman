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
  it("renders sc-empty-state when no messages", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const emptyState = el.shadowRoot?.querySelector("sc-empty-state");
    expect(emptyState).toBeTruthy();
    el.remove();
  });

  it("has suggested prompt pills", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const pills = el.shadowRoot?.querySelectorAll(".prompt-pill") ?? [];
    expect(pills.length).toBeGreaterThanOrEqual(1);
    el.remove();
  });

  it("message list has role=log and aria-live", async () => {
    const el = document.createElement("sc-chat-view") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const messageList = el.shadowRoot?.querySelector("#message-list");
    expect(messageList?.getAttribute("role")).toBe("log");
    expect(messageList?.getAttribute("aria-live")).toBe("polite");
    el.remove();
  });
});
