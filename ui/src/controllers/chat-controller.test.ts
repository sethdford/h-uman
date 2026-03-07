import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import type { ReactiveControllerHost } from "lit";
import { ChatController, type ChatItem, type GatewayLike } from "./chat-controller.js";
import { ChatCache } from "./chat-cache.js";
import { exportAsJson, exportAsMarkdown } from "./chat-export.js";

function createMockHost(): {
  requestUpdate: ReturnType<typeof vi.fn>;
  addController: ReturnType<typeof vi.fn>;
} {
  return {
    requestUpdate: vi.fn(),
    addController: vi.fn(),
  };
}

function createMockGateway(overrides?: Partial<GatewayLike>): GatewayLike {
  return {
    request: vi.fn().mockResolvedValue(undefined),
    abort: vi.fn().mockResolvedValue(undefined),
    status: "connected",
    ...overrides,
  };
}

describe("ChatController", () => {
  describe("initialization", () => {
    it("starts with empty items and not waiting", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      expect(ctrl.items).toEqual([]);
      expect(ctrl.isWaiting).toBe(false);
    });
  });

  describe("send", () => {
    it("appends user message and calls gateway", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.send("Hello", "sess-1");

      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "Hello",
      });
      expect(gw.request).toHaveBeenCalledWith("chat.send", {
        message: "Hello",
        sessionKey: "sess-1",
      });
    });

    it("sets isWaiting", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      const sendPromise = ctrl.send("Hi", "sess-1");
      expect(ctrl.isWaiting).toBe(true);
      await sendPromise;
    });

    it("stores lastFailedMessage on error", async () => {
      const host = createMockHost();
      const gw = createMockGateway({
        request: vi.fn().mockRejectedValue(new Error("Network error")),
      });
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await expect(ctrl.send("Fail me", "sess-1")).rejects.toThrow("Network error");
      expect(ctrl.lastFailedMessage).toBe("Fail me");
      expect(ctrl.isWaiting).toBe(false);
    });
  });

  describe("handleEvent", () => {
    it("processes received events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("chat", { state: "received", message: "Echoed user msg", id: "r-1" });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "Echoed user msg",
      });
    });

    it("deduplicates received events matching recent user message", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "message", role: "user", content: "already here", ts: Date.now() }];

      ctrl.handleEvent("chat", { state: "received", message: "already here", id: "r-2" });
      expect(ctrl.items).toHaveLength(1);
    });

    it("processes chunk events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("chat", { state: "chunk", message: "Hello", id: "1" });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "assistant",
        content: "Hello",
      });

      ctrl.handleEvent("chat", { state: "chunk", message: " world", id: "1" });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "assistant",
        content: "Hello world",
      });
    });

    it("processes sent events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("chat", {
        state: "sent",
        message: "Full response",
        id: "msg-1",
      });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "assistant",
        content: "Full response",
      });
      expect(ctrl.isWaiting).toBe(false);
    });

    it("processes tool_call events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("agent.tool", {
        id: "tc-1",
        message: "run_shell",
        input: '{"cmd":"ls"}',
      });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "tool_call",
        id: "tc-1",
        name: "run_shell",
        input: '{"cmd":"ls"}',
        status: "running",
      });

      ctrl.handleEvent("agent.tool", {
        id: "tc-1",
        message: "run_shell",
        result: "file1.txt\nfile2.txt",
      });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "tool_call",
        id: "tc-1",
        status: "completed",
        result: "file1.txt\nfile2.txt",
      });
    });

    it("processes thinking events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("thinking", { message: "Let me think" });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "thinking",
        content: "Let me think",
        streaming: true,
      });

      ctrl.handleEvent("thinking", { message: " about this..." });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "thinking",
        content: "Let me think about this...",
        streaming: true,
      });
    });

    it("sets errorBanner on error event", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      ctrl.handleEvent("error", { message: "Something went wrong" });
      expect(ctrl.errorBanner).toBe("Something went wrong");
    });
  });

  describe("abort", () => {
    it("calls gateway.abort and clears waiting", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.isWaiting = true;

      await ctrl.abort();

      expect(gw.abort).toHaveBeenCalled();
      expect(ctrl.isWaiting).toBe(false);
      expect(ctrl.streamElapsed).toBe("");
    });
  });

  describe("retry", () => {
    it("re-sends last failed message", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.lastFailedMessage = "Retry this";

      await ctrl.retry("sess-2");

      expect(gw.request).toHaveBeenCalledWith("chat.send", {
        message: "Retry this",
        sessionKey: "sess-2",
      });
      expect(ctrl.items).toHaveLength(1);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "Retry this",
      });
    });

    it("does nothing when lastFailedMessage is empty", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.retry("sess-1");

      expect(gw.request).not.toHaveBeenCalled();
    });
  });

  describe("cacheMessages / restoreFromCache", () => {
    beforeEach(() => {
      sessionStorage.clear();
    });

    afterEach(() => {
      sessionStorage.clear();
    });

    it("round-trips items via sessionStorage", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      const items: ChatItem[] = [
        { type: "message", role: "user", content: "Hi", ts: 1000 },
        {
          type: "message",
          role: "assistant",
          content: "Hello!",
          ts: 2000,
        },
      ];
      ctrl.items = items;
      ctrl.cacheMessages("test-session");

      const ctrl2 = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      const restored = ctrl2.restoreFromCache("test-session");

      expect(restored).toBe(true);
      expect(ctrl2.items).toHaveLength(2);
      expect(ctrl2.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "Hi",
      });
      expect(ctrl2.items[1]).toMatchObject({
        type: "message",
        role: "assistant",
        content: "Hello!",
      });
    });

    it("returns false when cache is empty", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      const restored = ctrl.restoreFromCache("nonexistent");
      expect(restored).toBe(false);
      expect(ctrl.items).toEqual([]);
    });
  });

  describe("loadHistory", () => {
    it("fetches from gateway when messages exist", async () => {
      const host = createMockHost();
      const gw = createMockGateway({
        request: vi.fn().mockResolvedValue({
          messages: [
            { role: "user", content: "First" },
            { role: "assistant", content: "Second" },
          ],
        }),
      });
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.loadHistory("hist-session");

      expect(gw.request).toHaveBeenCalledWith("chat.history", {
        sessionKey: "hist-session",
      });
      expect(ctrl.items).toHaveLength(2);
      expect(ctrl.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "First",
      });
      expect(ctrl.items[1]).toMatchObject({
        type: "message",
        role: "assistant",
        content: "Second",
      });
    });

    it("falls back to cache when gateway returns empty", async () => {
      const host = createMockHost();
      const gw = createMockGateway({
        request: vi.fn().mockResolvedValue({ messages: [] }),
      });
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      // Pre-populate cache
      ctrl.items = [{ type: "message", role: "user", content: "Cached", ts: 1 }];
      ctrl.cacheMessages("fallback-session");

      const ctrl2 = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      await ctrl2.loadHistory("fallback-session");

      expect(ctrl2.items).toHaveLength(1);
      expect(ctrl2.items[0]).toMatchObject({
        type: "message",
        role: "user",
        content: "Cached",
      });
    });

    it("does nothing when gateway is null", async () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.loadHistory("sess-1");

      expect(ctrl.items).toEqual([]);
    });
  });

  describe("toggleReaction", () => {
    it("adds a new reaction to a message", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "message", role: "assistant", content: "Hi", ts: 1 }];

      ctrl.toggleReaction(0, "👍");

      const item = ctrl.items[0];
      expect(item.type).toBe("message");
      if (item.type === "message") {
        expect(item.reactions).toHaveLength(1);
        expect(item.reactions![0]).toEqual({ emoji: "👍", count: 1, mine: true });
      }
    });

    it("removes own reaction when toggled again", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [
        {
          type: "message",
          role: "assistant",
          content: "Hi",
          ts: 1,
          reactions: [{ emoji: "❤️", count: 1, mine: true }],
        },
      ];

      ctrl.toggleReaction(0, "❤️");

      const item = ctrl.items[0];
      if (item.type === "message") {
        expect(item.reactions).toHaveLength(0);
      }
    });

    it("does nothing for out-of-bounds index", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "message", role: "user", content: "Hi", ts: 1 }];

      ctrl.toggleReaction(5, "👍");
      ctrl.toggleReaction(-1, "👍");

      const item = ctrl.items[0];
      if (item.type === "message") {
        expect(item.reactions).toBeUndefined();
      }
    });

    it("ignores non-message items", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "tool_call", id: "t1", name: "shell", status: "running", ts: 1 }];

      ctrl.toggleReaction(0, "👍");

      expect(ctrl.items[0].type).toBe("tool_call");
    });
  });

  describe("exportAsMarkdown", () => {
    it("formats messages with role labels", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [
        { type: "message", role: "user", content: "What is 2+2?", ts: 1 },
        { type: "message", role: "assistant", content: "4", ts: 2 },
      ];

      const md = ctrl.exportAsMarkdown();

      expect(md).toContain("**You**: What is 2+2?");
      expect(md).toContain("**Assistant**: 4");
    });

    it("includes tool calls", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [
        { type: "tool_call", id: "t1", name: "shell", status: "completed", result: "ok", ts: 1 },
      ];

      const md = ctrl.exportAsMarkdown();

      expect(md).toContain("Tool: shell");
      expect(md).toContain("ok");
    });

    it("returns empty string for empty items", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      expect(ctrl.exportAsMarkdown()).toBe("");
    });
  });

  describe("exportAsJson", () => {
    it("returns valid JSON of items", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "message", role: "user", content: "Hi", ts: 1 }];

      const json = ctrl.exportAsJson();
      const parsed = JSON.parse(json);

      expect(parsed).toHaveLength(1);
      expect(parsed[0].content).toBe("Hi");
    });
  });

  describe("getBranchMessages", () => {
    it("returns messages matching the given id", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [
        { type: "message", role: "user", content: "First", id: "m1", ts: 1 },
        { type: "message", role: "assistant", content: "Reply", id: "m2", ts: 2 },
        { type: "message", role: "user", content: "Branch", id: "m1", ts: 3 },
      ];

      const branches = ctrl.getBranchMessages("m1");

      expect(branches).toHaveLength(2);
      expect(branches[0]).toMatchObject({ content: "First" });
      expect(branches[1]).toMatchObject({ content: "Branch" });
    });

    it("returns empty array for unknown id", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);
      ctrl.items = [{ type: "message", role: "user", content: "Hi", id: "m1", ts: 1 }];

      expect(ctrl.getBranchMessages("nonexistent")).toEqual([]);
    });
  });

  describe("ChatCache (standalone)", () => {
    beforeEach(() => sessionStorage.clear());
    afterEach(() => sessionStorage.clear());

    it("save/restore round-trips items", () => {
      const items: ChatItem[] = [
        { type: "message", role: "user", content: "Hi", ts: 1000 },
        { type: "message", role: "assistant", content: "Hello!", ts: 2000 },
      ];
      ChatCache.save("cache-key", items);
      const restored = ChatCache.restore("cache-key");
      expect(restored).toHaveLength(2);
      expect(restored[0]).toMatchObject({ type: "message", role: "user", content: "Hi" });
      expect(restored[1]).toMatchObject({ type: "message", role: "assistant", content: "Hello!" });
    });

    it("restore returns empty array for missing key", () => {
      expect(ChatCache.restore("nonexistent")).toEqual([]);
    });

    it("clear removes cached data", () => {
      ChatCache.save("clear-key", [{ type: "message", role: "user", content: "x", ts: 1 }]);
      ChatCache.clear("clear-key");
      expect(ChatCache.restore("clear-key")).toEqual([]);
    });
  });

  describe("chat-export (standalone)", () => {
    it("exportAsMarkdown formats messages", () => {
      const items: ChatItem[] = [
        { type: "message", role: "user", content: "Q?", ts: 1 },
        { type: "message", role: "assistant", content: "A", ts: 2 },
      ];
      const md = exportAsMarkdown(items);
      expect(md).toContain("**You**: Q?");
      expect(md).toContain("**Assistant**: A");
    });

    it("exportAsJson returns valid JSON", () => {
      const items: ChatItem[] = [{ type: "message", role: "user", content: "Hi", ts: 1 }];
      const json = exportAsJson(items);
      const parsed = JSON.parse(json);
      expect(parsed).toHaveLength(1);
      expect(parsed[0].content).toBe("Hi");
    });
  });

  describe("stream timer", () => {
    beforeEach(() => {
      vi.useFakeTimers();
    });

    afterEach(() => {
      vi.useRealTimers();
    });

    it("updates streamElapsed every second", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.send("Hi", "sess-1");
      expect(ctrl.streamElapsed).toBe("0s");

      vi.advanceTimersByTime(1000);
      expect(ctrl.streamElapsed).toBe("1s");

      vi.advanceTimersByTime(2000);
      expect(ctrl.streamElapsed).toBe("3s");
    });

    it("stops timer on abort", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(host as unknown as ReactiveControllerHost, getGateway);

      await ctrl.send("Hi", "sess-1");
      vi.advanceTimersByTime(1000);
      expect(ctrl.streamElapsed).toBe("1s");

      await ctrl.abort();
      expect(ctrl.streamElapsed).toBe("");
      vi.advanceTimersByTime(5000);
      expect(ctrl.streamElapsed).toBe("");
    });
  });
});
