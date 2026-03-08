import type { ReactiveController, ReactiveControllerHost } from "lit";
import { EVENT_NAMES } from "../utils.js";
import { ChatCache } from "./chat-cache.js";
import {
  exportAsJson as exportItemsAsJson,
  exportAsMarkdown as exportItemsAsMarkdown,
} from "./chat-export.js";

export type MessageStatus = "sending" | "sent" | "streaming" | "complete" | "failed";
export interface Reaction {
  value: string;
  count: number;
  mine: boolean;
}

export type ChatItem =
  | {
      type: "message";
      role: "user" | "assistant";
      content: string;
      id?: string;
      ts?: number;
      status?: MessageStatus;
      editedFrom?: string;
      branchIndex?: number;
      branchCount?: number;
      reactions?: Reaction[];
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

const MAX_VISIBLE_ITEMS = 500;

/** Duration (ms) of the "completing" state after streaming ends. */
const COMPLETING_DURATION_MS = 400;

export class ChatController implements ReactiveController {
  items: ChatItem[] = [];
  trimmedCount = 0;
  isWaiting = false;
  isCompleting = false;
  lastFailedMessage = "";
  errorBanner = "";
  streamElapsed = "";
  historyLoading = false;
  historyError = "";
  loadingEarlier = false;

  private _getGateway: () => GatewayLike | null;
  private _streamStartTime = 0;
  private _streamTimer = 0;
  private _completingTimer = 0;

  constructor(
    private host: ReactiveControllerHost,
    getGateway: () => GatewayLike | null,
  ) {
    this._getGateway = getGateway;
    host.addController(this);
  }

  get hasEarlierMessages(): boolean {
    return this.trimmedCount > 0;
  }

  hostConnected(): void {
    /* no-op */
  }

  loadEarlier(): void {
    this.loadingEarlier = true;
    this._requestUpdate();
    (this.host as unknown as EventTarget).dispatchEvent(
      new CustomEvent("sc-load-earlier-request", { bubbles: true, composed: true }),
    );
    // Stub: gateway integration will clear loadingEarlier when done
  }

  private _trimIfNeeded(): void {
    if (this.items.length > MAX_VISIBLE_ITEMS) {
      const excess = this.items.length - MAX_VISIBLE_ITEMS;
      this.items = this.items.slice(excess);
      this.trimmedCount += excess;
    }
  }

  hostDisconnected(): void {
    this._stopStreamTimer();
    if (this._completingTimer) {
      window.clearTimeout(this._completingTimer);
      this._completingTimer = 0;
    }
  }

  async send(
    text: string,
    sessionKey: string,
    attachments?: Array<{ name: string; type: string; data: string }>,
  ): Promise<void> {
    const gw = this._getGateway();
    if (!gw) return;

    const userMsg: ChatItem = {
      type: "message",
      role: "user",
      content: text,
      ts: Date.now(),
      status: "sending",
    };
    this.items = [...this.items, userMsg];
    this._trimIfNeeded();
    this.lastFailedMessage = "";
    this.isWaiting = true;
    this._startStreamTimer();
    this.cacheMessages(sessionKey);
    this._requestUpdate();

    try {
      await gw.request("chat.send", {
        message: text,
        sessionKey,
        ...(attachments?.length ? { attachments } : {}),
      });
      this._setLastUserStatus("sent");
    } catch (err) {
      this.isWaiting = false;
      this._stopStreamTimer();
      this._setLastUserStatus("failed");
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
      } catch (e) {
        console.warn("[chat-controller] abort failed:", e);
      }
    }
    this.isWaiting = false;
    this._stopStreamTimer();
    this._requestUpdate();
  }

  async retry(sessionKey: string): Promise<void> {
    const msg = this.lastFailedMessage || this._findLastFailedContent();
    if (!msg) return;
    const failedIdx = this._findLastFailedIndex();
    if (failedIdx >= 0) {
      this.items = [...this.items.slice(0, failedIdx), ...this.items.slice(failedIdx + 1)];
    }
    this.lastFailedMessage = "";
    await this.send(msg, sessionKey);
  }

  private _findLastFailedContent(): string {
    const idx = this._findLastFailedIndex();
    if (idx < 0) return "";
    const item = this.items[idx];
    return item.type === "message" ? item.content : "";
  }

  private _findLastFailedIndex(): number {
    for (let i = this.items.length - 1; i >= 0; i--) {
      const item = this.items[i];
      if (item.type === "message" && item.role === "user" && item.status === "failed") {
        return i;
      }
    }
    return -1;
  }

  async loadHistory(sessionKey: string): Promise<void> {
    const gw = this._getGateway();
    if (!gw) return;

    this.historyLoading = true;
    this.historyError = "";
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
        this._trimIfNeeded();
        this.cacheMessages(sessionKey);
        this._requestUpdate();
        return;
      }
    } catch (err) {
      this.historyError =
        err instanceof Error ? err.message : "Failed to load conversation history";
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
    ChatCache.save(sessionKey, this.items);
  }

  restoreFromCache(sessionKey: string): boolean {
    const items = ChatCache.restore(sessionKey);
    if (items.length === 0) return false;
    this.items = items;
    this._trimIfNeeded();
    return true;
  }

  private _beginCompleting(sessionKey: string): void {
    this.isWaiting = false;
    this.isCompleting = true;
    this._stopStreamTimer();
    this._requestUpdate();

    this._completingTimer = window.setTimeout(() => {
      this._completingTimer = 0;
      this.isCompleting = false;
      this.cacheMessages(sessionKey);
      this._requestUpdate();
    }, COMPLETING_DURATION_MS);
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

  private _setLastUserStatus(status: MessageStatus): void {
    for (let i = this.items.length - 1; i >= 0; i--) {
      const item = this.items[i];
      if (item.type === "message" && item.role === "user") {
        this.items = [...this.items.slice(0, i), { ...item, status }, ...this.items.slice(i + 1)];
        return;
      }
    }
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
      this._trimIfNeeded();
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
        this._trimIfNeeded();
      }
    }

    if (state === "sent" && content) {
      this.items = this.items.map((i) =>
        i.type === "thinking" && i.streaming ? { ...i, streaming: false } : i,
      );
      const lastMsgIdx = this._findLastAssistantIdx();
      if (lastMsgIdx >= 0) {
        const last = this.items[lastMsgIdx];
        if (last.type === "message" && last.role === "assistant") {
          this.items = [
            ...this.items.slice(0, lastMsgIdx),
            { ...last, content },
            ...this.items.slice(lastMsgIdx + 1),
          ];
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
          this._trimIfNeeded();
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
        this._trimIfNeeded();
      }
      this._beginCompleting(sessionKey);
      this._requestUpdate();
      return;
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
        this._trimIfNeeded();
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
      this._trimIfNeeded();
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

  toggleReaction(index: number, value: string): void {
    if (index < 0 || index >= this.items.length) return;
    const item = this.items[index];
    if (item.type !== "message") return;
    const reactions = [...(item.reactions ?? [])];
    const existing = reactions.findIndex((r) => r.value === value);
    if (existing >= 0) {
      if (reactions[existing].mine) {
        reactions[existing] = {
          ...reactions[existing],
          count: reactions[existing].count - 1,
          mine: false,
        };
        if (reactions[existing].count <= 0) reactions.splice(existing, 1);
      } else {
        reactions[existing] = {
          ...reactions[existing],
          count: reactions[existing].count + 1,
          mine: true,
        };
      }
    } else {
      reactions.push({ value, count: 1, mine: true });
    }
    this.items = [
      ...this.items.slice(0, index),
      { ...item, reactions },
      ...this.items.slice(index + 1),
    ];
    this._requestUpdate();
  }

  getBranchMessages(messageId: string): ChatItem[] {
    return this.items.filter((i) => i.type === "message" && i.id === messageId);
  }

  exportAsMarkdown(): string {
    return exportItemsAsMarkdown(this.items);
  }

  exportAsJson(): string {
    return exportItemsAsJson(this.items);
  }
}
