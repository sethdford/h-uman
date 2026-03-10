import { describe, it, expect, vi } from "vitest";
import "./hu-chat-sessions-panel.js";

type SessionsPanelEl = HTMLElement & {
  sessions: Array<{ id: string; title: string; ts: number; active: boolean }>;
  open: boolean;
  updateComplete: Promise<boolean>;
};

describe("hu-chat-sessions-panel", () => {
  it("registers as custom element", () => {
    expect(customElements.get("hu-chat-sessions-panel")).toBeDefined();
  });

  it("renders session items", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
    el.sessions = [
      { id: "s1", title: "First chat", ts: Date.now(), active: false },
      { id: "s2", title: "Second chat", ts: Date.now() - 86400000, active: false },
    ];
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const items = el.shadowRoot?.querySelectorAll(".session-item") ?? [];
    expect(items.length).toBe(2);
    el.remove();
  });

  it("highlights active session", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
    el.sessions = [{ id: "s1", title: "Active", ts: Date.now(), active: true }];
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const active = el.shadowRoot?.querySelector(".session-item.active");
    expect(active).toBeTruthy();
    el.remove();
  });

  it("fires hu-session-select on click", async () => {
    const onSelect = vi.fn();
    const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
    el.sessions = [{ id: "s1", title: "Test", ts: Date.now(), active: false }];
    el.open = true;
    el.addEventListener("hu-session-select", onSelect);
    document.body.appendChild(el);
    await el.updateComplete;
    const item = el.shadowRoot?.querySelector(".session-item") as HTMLElement;
    item?.click();
    expect(onSelect).toHaveBeenCalledTimes(1);
    el.remove();
  });

  it("fires hu-session-new on new chat button", async () => {
    const onNew = vi.fn();
    const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
    el.sessions = [];
    el.open = true;
    el.addEventListener("hu-session-new", onNew);
    document.body.appendChild(el);
    await el.updateComplete;
    const btn = el.shadowRoot?.querySelector(".new-chat-btn") as HTMLButtonElement;
    btn?.click();
    expect(onNew).toHaveBeenCalledTimes(1);
    el.remove();
  });

  it("groups sessions by time", async () => {
    const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
    el.sessions = [
      { id: "s1", title: "Today", ts: Date.now(), active: false },
      { id: "s2", title: "Yesterday", ts: Date.now() - 86400000, active: false },
      { id: "s3", title: "Last week", ts: Date.now() - 5 * 86400000, active: false },
    ];
    el.open = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const groups = el.shadowRoot?.querySelectorAll(".group-label") ?? [];
    expect(groups.length).toBeGreaterThanOrEqual(2);
    el.remove();
  });
});
