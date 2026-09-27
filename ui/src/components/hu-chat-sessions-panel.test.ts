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

  describe("keyboard", () => {
    const now = Date.now();
    async function mount(sessions: SessionsPanelEl["sessions"]) {
      const el = document.createElement("hu-chat-sessions-panel") as SessionsPanelEl;
      el.sessions = sessions;
      el.open = true;
      document.body.appendChild(el);
      await el.updateComplete;
      const root = el.shadowRoot!;
      const open = () => Array.from(root.querySelectorAll<HTMLButtonElement>(".session-open"));
      const focusedId = () =>
        (root.activeElement as HTMLElement | null)?.closest<HTMLElement>(".session-item")?.dataset
          .sessionId;
      const key = async (k: string) => {
        (root.activeElement ?? root.querySelector(".session-list"))!.dispatchEvent(
          new KeyboardEvent("keydown", { key: k, bubbles: true, composed: true, cancelable: true }),
        );
        await el.updateComplete;
      };
      return { el, root, open, focusedId, key };
    }
    const three = (activeIdx = -1) =>
      ["a", "b", "c"].map((id, i) => ({
        id,
        title: `chat ${id}`,
        ts: now - i * 1000,
        active: i === activeIdx,
      }));

    it("uses list semantics, not an interactive container", async () => {
      const { el, root } = await mount(three());
      expect(root.querySelector('[role="listbox"], [role="option"]')).toBeNull();
      const list = root.querySelector('[role="list"]')!;
      expect(list.querySelectorAll('[role="listitem"]').length).toBe(3);
      const labelId = list.getAttribute("aria-labelledby")!;
      expect(root.getElementById(labelId)?.textContent).toBe("Today");
      el.remove();
    });

    it("exposes exactly one row to Tab, the active session", async () => {
      const { el, root } = await mount(three(1));
      const tabbable = Array.from(
        root.querySelectorAll<HTMLElement>('.session-item [tabindex="0"]'),
      );
      expect(
        tabbable.map((b) => b.closest<HTMLElement>(".session-item")!.dataset.sessionId),
      ).toEqual(["b", "b"]);
      expect(tabbable.map((b) => b.className)).toEqual(["session-open", "delete-btn"]);
      el.remove();
    });

    it("falls back to the first row when no session is active", async () => {
      const { el, open } = await mount(three());
      expect(open().map((b) => b.tabIndex)).toEqual([0, -1, -1]);
      el.remove();
    });

    it("arrow, Home and End keys move real focus and the tab stop with it", async () => {
      const { el, open, focusedId, key } = await mount(three());
      open()[0].focus();
      await key("ArrowDown");
      expect(focusedId()).toBe("b");
      expect(open().map((b) => b.tabIndex)).toEqual([-1, 0, -1]);
      await key("End");
      expect(focusedId()).toBe("c");
      await key("ArrowDown");
      expect(focusedId()).toBe("c");
      await key("Home");
      expect(focusedId()).toBe("a");
      await key("ArrowUp");
      expect(focusedId()).toBe("a");
      el.remove();
    });

    it("Enter on the focused row selects it", async () => {
      const { el, open } = await mount(three());
      const onSelect = vi.fn();
      el.addEventListener("hu-session-select", onSelect);
      open()[2].focus();
      // A button's Enter activation is a click; the row's handler selects.
      open()[2].click();
      expect(onSelect).toHaveBeenCalledTimes(1);
      expect((onSelect.mock.calls[0][0] as CustomEvent).detail).toEqual({ id: "c" });
      el.remove();
    });

    for (const k of ["Delete", "Backspace"]) {
      it(`${k} deletes the focused row and focus moves to its neighbour`, async () => {
        const { el, open, focusedId, key } = await mount(three());
        el.addEventListener("hu-session-delete", (e) => {
          const id = (e as CustomEvent<{ id: string }>).detail.id;
          el.sessions = el.sessions.filter((s) => s.id !== id);
        });
        open()[1].focus();
        await key(k);
        await el.updateComplete;
        expect(el.sessions.map((s) => s.id)).toEqual(["a", "c"]);
        expect(focusedId()).toBe("c");
        expect(open().map((b) => b.tabIndex)).toEqual([-1, 0]);
        el.remove();
      });
    }

    it("Delete on the last row moves focus to the previous row", async () => {
      const { el, open, focusedId, key } = await mount(three());
      el.addEventListener("hu-session-delete", (e) => {
        const id = (e as CustomEvent<{ id: string }>).detail.id;
        el.sessions = el.sessions.filter((s) => s.id !== id);
      });
      open()[2].focus();
      await key("Delete");
      await el.updateComplete;
      expect(focusedId()).toBe("b");
      el.remove();
    });

    it("Backspace while renaming a title edits text instead of deleting", async () => {
      const { el, root, key } = await mount(three());
      const onDelete = vi.fn();
      el.addEventListener("hu-session-delete", onDelete);
      const title = root.querySelector<HTMLElement>(".session-title")!;
      title.contentEditable = "true";
      title.focus();
      await key("Backspace");
      expect(onDelete).not.toHaveBeenCalled();
      el.remove();
    });
  });
});
