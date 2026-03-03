import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { GatewayClient } from "./gateway.js";

const mockWsInstances: MockWebSocket[] = [];

class MockWebSocket {
  static readonly OPEN = 1;
  readyState = MockWebSocket.OPEN;
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((ev: { data: string }) => void) | null = null;
  send = vi.fn();
  close = vi.fn();

  constructor(_url?: string | URL, _protocols?: string | string[]) {
    mockWsInstances.push(this);
  }

  /** Simulate connection open (call after client has set onopen) */
  simulateOpen(): void {
    this.onopen?.();
  }

  /** Simulate receiving a message */
  simulateMessage(data: unknown): void {
    const str = typeof data === "string" ? data : JSON.stringify(data);
    this.onmessage?.({ data: str });
  }
}

const OriginalWebSocket = globalThis.WebSocket;

beforeEach(() => {
  mockWsInstances.length = 0;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).WebSocket = MockWebSocket;
});

afterEach(() => {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).WebSocket = OriginalWebSocket;
});

async function connectClient(client: GatewayClient, url = "ws://test") {
  client.connect(url);
  const ws = mockWsInstances[mockWsInstances.length - 1]!;
  ws.simulateOpen();
  await new Promise((r) => setTimeout(r, 0));
  // Respond to the automatic "connect" handshake if one was sent
  if (ws.send.mock.calls.length > 0) {
    const handshake = JSON.parse(ws.send.mock.calls[0]![0] as string);
    if (handshake.method === "connect") {
      ws.simulateMessage({
        id: handshake.id,
        type: "res",
        ok: true,
        payload: { features: {} },
      });
      await new Promise((r) => setTimeout(r, 0));
    }
  }
  ws.send.mockClear();
  return ws;
}

describe("GatewayClient", () => {
  describe("initial state", () => {
    it('initial status is "disconnected"', () => {
      const client = new GatewayClient();
      expect(client.status).toBe("disconnected");
    });
  });

  describe("connect", () => {
    it('changes status to "connecting" then "connected" on open', async () => {
      const client = new GatewayClient();
      client.connect("ws://example.com");

      expect(client.status).toBe("connecting");

      const ws = mockWsInstances[0]!;
      ws.simulateOpen();
      await new Promise((r) => setTimeout(r, 0));

      expect(client.status).toBe("connected");
    });
  });

  describe("disconnect", () => {
    it('sets status to "disconnected"', async () => {
      const client = new GatewayClient();
      await connectClient(client);

      expect(client.status).toBe("connected");
      client.disconnect();
      expect(client.status).toBe("disconnected");
    });

    it("clears pending requests", async () => {
      const client = new GatewayClient();
      await connectClient(client);

      const reqPromise = client.request("test.method", {}, 5000);
      client.disconnect();

      await expect(reqPromise).rejects.toThrow("Disconnected");
    });
  });

  describe("request", () => {
    it("rejects when not connected", async () => {
      const client = new GatewayClient();
      await expect(client.request("foo")).rejects.toThrow("WebSocket not connected");
    });

    it("sends JSON-RPC message via WebSocket", async () => {
      const client = new GatewayClient();
      const ws = await connectClient(client);

      const resPromise = client.request("test.method", { foo: "bar" });
      const sent = JSON.parse(ws.send.mock.calls[0]![0] as string);
      ws.simulateMessage({
        id: sent.id,
        type: "res",
        ok: true,
        result: { done: true },
      });
      await resPromise;

      expect(ws.send).toHaveBeenCalled();
      expect(sent.type).toBe("req");
      expect(sent.method).toBe("test.method");
      expect(sent.params).toEqual({ foo: "bar" });
      expect(sent.id).toBeDefined();
    });

    it("resolves when response arrives", async () => {
      const client = new GatewayClient();
      const ws = await connectClient(client);

      const resPromise = client.request<{ value: number }>("getValue");
      const reqId = JSON.parse(ws.send.mock.calls[ws.send.mock.calls.length - 1]![0]).id;
      ws.simulateMessage({
        id: reqId,
        type: "res",
        ok: true,
        result: { value: 42 },
      });

      const result = await resPromise;
      expect(result).toEqual({ value: 42 });
    });

    it("rejects on error response", async () => {
      const client = new GatewayClient();
      const ws = await connectClient(client);

      const resPromise = client.request("failMethod");
      const reqId = JSON.parse(ws.send.mock.calls[ws.send.mock.calls.length - 1]![0]).id;
      ws.simulateMessage({
        id: reqId,
        type: "res",
        ok: false,
        error: { code: -32000, message: "Something went wrong" },
      });

      await expect(resPromise).rejects.toThrow("Something went wrong");
    });

    it("rejects on timeout", async () => {
      vi.useFakeTimers();
      const client = new GatewayClient();
      await connectClient(client);

      const resPromise = client.request("slowMethod", {}, 100);
      vi.advanceTimersByTime(150);

      await expect(resPromise).rejects.toThrow("Request timeout");
      vi.useRealTimers();
    });
  });

  describe("status events", () => {
    it("dispatches status events on connect/disconnect", async () => {
      const statuses: string[] = [];
      const client = new GatewayClient();
      client.addEventListener(GatewayClient.EVENT_STATUS, ((e: CustomEvent) => {
        statuses.push(e.detail);
      }) as EventListener);

      client.connect("ws://test");
      expect(statuses).toContain("connecting");

      mockWsInstances[0]!.simulateOpen();
      await new Promise((r) => setTimeout(r, 0));
      expect(statuses).toContain("connected");

      client.disconnect();
      expect(statuses).toContain("disconnected");
    });
  });

  describe("features", () => {
    it("parses features from hello-ok message", async () => {
      const client = new GatewayClient();
      const ws = await connectClient(client);

      ws.simulateMessage({
        type: "hello-ok",
        features: {
          methods: ["chat.send", "sessions.list"],
          sessions: true,
          cron: true,
        },
      });

      expect(client.features).toEqual({
        methods: ["chat.send", "sessions.list"],
        sessions: true,
        cron: true,
      });
    });
  });
});
