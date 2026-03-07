import { describe, it, expect, vi } from "vitest";
import "./sc-message-list.js";
import type { ChatItem } from "../controllers/chat-controller.js";

type ScMessageListEl = HTMLElement & {
  items: ChatItem[];
  isWaiting: boolean;
  streamElapsed: string;
  historyLoading: boolean;
  scrollToBottom: () => void;
  scrollToItem: (idx: number) => void;
  updateComplete: Promise<boolean>;
};

describe("sc-message-list", () => {
  it("registers as custom element", () => {
    expect(customElements.get("sc-message-list")).toBeDefined();
  });

  it("has role=log and aria-live=polite", async () => {
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    document.body.appendChild(el);
    await el.updateComplete;
    const scrollContainer = el.shadowRoot?.querySelector("#scroll-container");
    expect(scrollContainer?.getAttribute("role")).toBe("log");
    expect(scrollContainer?.getAttribute("aria-live")).toBe("polite");
    expect(scrollContainer?.getAttribute("aria-label")).toBe("Chat messages");
    el.remove();
  });

  it("renders message items with sc-message-stream", async () => {
    const items: ChatItem[] = [
      { type: "message", role: "user", content: "Hello", ts: Date.now() },
      { type: "message", role: "assistant", content: "Hi there", ts: Date.now() },
    ];
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = items;
    document.body.appendChild(el);
    await el.updateComplete;
    const streams = el.shadowRoot?.querySelectorAll("sc-message-stream");
    expect(streams?.length).toBe(2);
    expect((streams?.[0] as { content?: string })?.content).toBe("Hello");
    el.remove();
  });

  it("renders tool_call items with sc-tool-result", async () => {
    const items: ChatItem[] = [
      {
        type: "tool_call",
        id: "t1",
        name: "fetch",
        status: "completed",
        result: "OK",
      },
    ];
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = items;
    document.body.appendChild(el);
    await el.updateComplete;
    const toolResult = el.shadowRoot?.querySelector("sc-tool-result");
    expect(toolResult).toBeTruthy();
    el.remove();
  });

  it("renders thinking items with sc-reasoning-block", async () => {
    const items: ChatItem[] = [{ type: "thinking", content: "Let me think...", streaming: false }];
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = items;
    document.body.appendChild(el);
    await el.updateComplete;
    const reasoning = el.shadowRoot?.querySelector("sc-reasoning-block");
    expect(reasoning).toBeTruthy();
    el.remove();
  });

  it("shows thinking indicator when isWaiting", async () => {
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = [];
    el.isWaiting = true;
    el.streamElapsed = "5s";
    document.body.appendChild(el);
    await el.updateComplete;
    const thinking = el.shadowRoot?.querySelector(".thinking");
    const scThinking = el.shadowRoot?.querySelector("sc-thinking");
    const elapsed = el.shadowRoot?.querySelector(".stream-elapsed");
    expect(thinking).toBeTruthy();
    expect(scThinking).toBeTruthy();
    expect(elapsed?.textContent).toBe("5s");
    el.remove();
  });

  it("exposes scrollToBottom method", async () => {
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    document.body.appendChild(el);
    await el.updateComplete;
    expect(typeof el.scrollToBottom).toBe("function");
    expect(() => el.scrollToBottom()).not.toThrow();
    el.remove();
  });

  it("fires sc-context-menu on right-click", async () => {
    const items: ChatItem[] = [{ type: "message", role: "user", content: "Test", ts: Date.now() }];
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = items;
    document.body.appendChild(el);
    await el.updateComplete;
    const onContextMenu = vi.fn();
    el.addEventListener("sc-context-menu", onContextMenu);
    const msgDiv = el.shadowRoot?.querySelector("#msg-0");
    expect(msgDiv).toBeTruthy();
    msgDiv?.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true }));
    expect(onContextMenu).toHaveBeenCalledTimes(1);
    expect(onContextMenu.mock.calls[0][0].detail.item).toEqual(items[0]);
    el.remove();
  });

  it("fires sc-abort when abort clicked", async () => {
    const el = document.createElement("sc-message-list") as ScMessageListEl;
    el.items = [];
    el.isWaiting = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const onAbort = vi.fn();
    el.addEventListener("sc-abort", onAbort);
    const abortBtn = el.shadowRoot?.querySelector(".abort-btn");
    expect(abortBtn).toBeTruthy();
    (abortBtn as HTMLButtonElement).click();
    expect(onAbort).toHaveBeenCalledTimes(1);
    el.remove();
  });
});
