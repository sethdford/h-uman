import type { ReactiveController, ReactiveControllerHost } from "lit";
import { EVENT_NAMES } from "../utils.js";

export type ChatItem =
  | {
      type: "message";
      role: "user" | "assistant";
      content: string;
      id?: string;
      ts?: number;
    }
  | {
      type: "tool_call";
      id: string;
      name: string;
      input?: string;
      status: "running" | "completed";
      result?: string;
      ts?: number;
    }
  | {
      type: "thinking";
      content: string;
      streaming: boolean;
      duration?: string;
      ts?: number;
    };

export interface GatewayLike {
  request<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T>;
  abort(): Promise<void>;
  status: string;
}

export class ChatController implements ReactiveController {
  items: ChatItem[] = [];
  isWaiting = false;
  lastFailedMessage = "";
  errorBanner = "";
  streamElapsed = "";
  historyLoading = false;

  private _getGateway: () => GatewayLike | null;
  private _streamStartTime = 0;
  private _streamTimer = 0;

  constructor(
    private host: ReactiveControllerHost,
    getGateway: () => GatewayLike | null,
  ) {
    this._getGateway = getGateway;
    host.addController(this);
  }

  hostConnected(): void {
    /* no-op */
  }

  hostDisconnected(): void {
    this._stopStreamTimer();
  }

  async send(text: string, sessionKey: string): Promise<void> {
    const gw = this._getGateway();
    if (!gw) return;

    this.items = [...this.items, { type: "message", role: "user", content: text, ts: Date.now() }];
    this.lastFailedMessage = "";
    this.isWaiting = true;
    this._startStreamTimer();
    this.cacheMessages(sessionKey);
    this._requestUpdate();

    try {
      await gw.request("chat.send", { message: text, sessionKey });
    } catch (err) {
      this.isWaiting = false;
      this._stopStreamTimer();
      this.lastFailedMessage = text;
      this._requestUpdate();
      throw err;
    }
  }

  async abort(): Promise<void> {
    const gw = this._getGateway();
    if (gw) {
      try {
        await gw.abort();
      } catch {
        /* abort is best-effort */
      }
    }
    this.isWaiting = false;
    this._stopStreamTimer();
    this._requestUpdate();
  }

  async retry(sessionKey: string): Promise<void> {
    if (!this.lastFailedMessage) return;
    const msg = this.lastFailedMessage;
    await this.send(msg, sessionKey);
  }

  async loadHistory(sessionKey: string): Promise<void> {
    const gw = this._getGateway();
    if (!gw) return;

    this.historyLoading = true;
    this._requestUpdate();
    try {
      const res = await gw.request<{
        messages?: { role: string; content: string }[];
      }>("chat.history", { sessionKey });
      if (res?.messages && Array.isArray(res.messages) && res.messages.length > 0) {
        this.items = res.messages.map((m) => ({
          type: "message",
          role: m.role as "user" | "assistant",
          content: m.content ?? "",
        }));
        this.cacheMessages(sessionKey);
        this._requestUpdate();
        return;
      }
    } catch {
      /* history load is best-effort */
    } finally {
      this.historyLoading = false;
      this._requestUpdate();
    }
    if (this.restoreFromCache(sessionKey)) {
      this._requestUpdate();
    }
  }

  handleEvent(event: string, payload: Record<string, unknown>, sessionKey = "default"): void {
    if (event === EVENT_NAMES.ERROR || event === "error") {
      const msg = (payload.message as string) ?? (payload.error as string) ?? "Unknown error";
      this.errorBanner = msg;
      this._requestUpdate();
      return;
    }

    if (
      event === "thinking" ||
      (event === EVENT_NAMES.CHAT && (payload.state as string) === "thinking")
    ) {
      this._handleThinking(payload, sessionKey);
      return;
    }

    if (event === EVENT_NAMES.CHAT) {
      this._handleChat(payload, sessionKey);
      return;
    }

    if (event === EVENT_NAMES.TOOL_CALL || event === "tool_call") {
      this._handleToolCall(payload, sessionKey);
    }
  }

  cacheMessages(sessionKey: string): void {
    try {
      const key = `sc-chat-${sessionKey}`;
      sessionStorage.setItem(key, JSON.stringify(this.items));
    } catch {
      /* quota exceeded — ignore */
    }
  }

  restoreFromCache(sessionKey: string): boolean {
    try {
      const key = `sc-chat-${sessionKey}`;
      const raw = sessionStorage.getItem(key);
      if (!raw) return false;
      const cached = JSON.parse(raw) as unknown;
      if (!Array.isArray(cached) || cached.length === 0) return false;
      this.items = cached
        .map((item: unknown) => {
          const obj = item as Record<string, unknown>;
          if (obj?.type === "message" || obj?.type === "tool_call" || obj?.type === "thinking") {
            return item as ChatItem;
          }
          if (obj?.role && obj?.content) {
            return {
              type: "message",
              role: obj.role as "user" | "assistant",
              content: String(obj.content ?? ""),
            } as ChatItem;
          }
          return null;
        })
        .filter((i): i is ChatItem => i != null);
      return this.items.length > 0;
    } catch {
      /* corrupt cache — ignore */
    }
    return false;
  }

  private _startStreamTimer(): void {
    this._streamStartTime = Date.now();
    this.streamElapsed = "0s";
    this._streamTimer = window.setInterval(() => {
      const elapsed = Math.floor((Date.now() - this._streamStartTime) / 1000);
      this.streamElapsed =
        elapsed < 60 ? `${elapsed}s` : `${Math.floor(elapsed / 60)}m ${elapsed % 60}s`;
      this._requestUpdate();
    }, 1000);
  }

  private _stopStreamTimer(): void {
    if (this._streamTimer) {
      window.clearInterval(this._streamTimer);
      this._streamTimer = 0;
    }
    this.streamElapsed = "";
  }

  private _requestUpdate(): void {
    this.host.requestUpdate();
  }

  private _handleThinking(payload: Record<string, unknown>, sessionKey: string): void {
    const content = (payload.message as string) ?? "";
    const streaming = this.items.filter(
      (i): i is Extract<ChatItem, { type: "thinking" }> => i.type === "thinking" && i.streaming,
    );
    const existingThinking = streaming.length > 0 ? streaming[streaming.length - 1]! : null;
    if (existingThinking) {
      this.items = this.items.map((i) =>
        i === existingThinking ? { ...i, content: i.content + content } : i,
      );
    } else {
      this.items = [...this.items, { type: "thinking", content, streaming: true, ts: Date.now() }];
    }
    this.cacheMessages(sessionKey);
    this._requestUpdate();
  }

  private _handleChat(payload: Record<string, unknown>, sessionKey: string): void {
    const state = payload.state as string;
    const content = (payload.message as string) ?? "";

    if (state === "received" && content) {
      const recentUser = this.items
        .slice(-6)
        .some((i) => i.type === "message" && i.role === "user" && i.content === content);
      if (!recentUser) {
        this.items = [
          ...this.items,
          {
            type: "message",
            role: "user" as const,
            content,
            id: payload.id as string,
            ts: Date.now(),
          },
        ];
      }
    }

    if (state === "sent" && content) {
      this.items = this.items.map((i) =>
        i.type === "thinking" && i.streaming ? { ...i, streaming: false } : i,
      );
      this.items = [
        ...this.items,
        {
          type: "message",
          role: "assistant" as const,
          content,
          id: payload.id as string,
          ts: Date.now(),
        },
      ];
      this.isWaiting = false;
      this._stopStreamTimer();
    }

    if (state === "chunk" && content) {
      this.items = this.items.map((i) =>
        i.type === "thinking" && i.streaming ? { ...i, streaming: false } : i,
      );
      const lastMsgIdx = this._findLastAssistantIdx();
      if (lastMsgIdx >= 0) {
        const last = this.items[lastMsgIdx];
        if (last.type === "message") {
          this.items = [
            ...this.items.slice(0, lastMsgIdx),
            { ...last, content: last.content + content },
            ...this.items.slice(lastMsgIdx + 1),
          ];
        }
      } else {
        this.items = [
          ...this.items,
          {
            type: "message",
            role: "assistant" as const,
            content,
            id: payload.id as string,
            ts: Date.now(),
          },
        ];
      }
    }

    if (state === "sent" && !content) {
      this.isWaiting = true;
      this._startStreamTimer();
    }

    this.cacheMessages(sessionKey);
    this._requestUpdate();
  }

  private _handleToolCall(payload: Record<string, unknown>, sessionKey: string): void {
    const id = (payload.id as string) ?? `tool-${Date.now()}`;
    const name = (payload.message as string) ?? "tool";
    const input =
      typeof payload.input === "string"
        ? payload.input
        : payload.args != null
          ? JSON.stringify(payload.args)
          : undefined;
    const result = payload.result != null ? String(payload.result) : undefined;
    const existingIdx = this.items.findIndex(
      (i): i is Extract<ChatItem, { type: "tool_call" }> => i.type === "tool_call" && i.id === id,
    );
    if (existingIdx < 0) {
      this.items = [
        ...this.items,
        {
          type: "tool_call",
          id,
          name,
          input,
          status: result != null ? "completed" : "running",
          result,
          ts: Date.now(),
        },
      ];
    } else {
      const existing = this.items[existingIdx];
      if (existing.type === "tool_call") {
        this.items = [
          ...this.items.slice(0, existingIdx),
          {
            ...existing,
            input: existing.input ?? input,
            status: "completed" as const,
            result: result ?? existing.result,
          },
          ...this.items.slice(existingIdx + 1),
        ];
      }
    }
    this.cacheMessages(sessionKey);
    this._requestUpdate();
  }

  private _findLastAssistantIdx(): number {
    for (let i = this.items.length - 1; i >= 0; i--) {
      if (
        this.items[i].type === "message" &&
        (this.items[i] as { role: string }).role === "assistant"
      ) {
        return i;
      }
    }
    return -1;
  }
}
