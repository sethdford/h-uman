import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { ChatController, type ChatItem, type GatewayLike } from "./chat-controller.js";

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );
      expect(ctrl.items).toEqual([]);
      expect(ctrl.isWaiting).toBe(false);
    });
  });

  describe("send", () => {
    it("appends user message and calls gateway", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

      await expect(ctrl.send("Fail me", "sess-1")).rejects.toThrow("Network error");
      expect(ctrl.lastFailedMessage).toBe("Fail me");
      expect(ctrl.isWaiting).toBe(false);
    });
  });

  describe("handleEvent", () => {
    it("processes chunk events", () => {
      const host = createMockHost();
      const getGateway = vi.fn().mockReturnValue(null);
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

      ctrl.handleEvent("error", { message: "Something went wrong" });
      expect(ctrl.errorBanner).toBe("Something went wrong");
    });
  });

  describe("abort", () => {
    it("calls gateway.abort and clears waiting", async () => {
      const host = createMockHost();
      const gw = createMockGateway();
      const getGateway = vi.fn().mockReturnValue(gw);
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );
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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );
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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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

      const ctrl2 = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );
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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

      // Pre-populate cache
      ctrl.items = [{ type: "message", role: "user", content: "Cached", ts: 1 }];
      ctrl.cacheMessages("fallback-session");

      const ctrl2 = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );
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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

      await ctrl.loadHistory("sess-1");

      expect(ctrl.items).toEqual([]);
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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
      const ctrl = new ChatController(
        host as unknown as import("lit").ReactiveControllerHost,
        getGateway,
      );

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
