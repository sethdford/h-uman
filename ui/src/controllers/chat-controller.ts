import type { ReactiveController, ReactiveControllerHost } from "lit";
import { EVENT_NAMES } from "../utils.js";
import { log } from "../lib/log.js";
import type { GatewayClient } from "../gateway.js";
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
      replyTo?: { id: string; content: string; role: "user" | "assistant" };
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
    }
  | {
      type: "memory";
      action: "recall" | "store" | "forget";
      key: string;
      value?: string;
      ts?: number;
    }
  | {
      type: "web_search";
      query: string;
      sites: string[];
      sources?: Array<{ title: string; url: string }>;
      ts?: number;
    };

export interface GatewayLike {
  request<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T>;
  abort(): Promise<void>;
  status: string;
}

export interface ArtifactData {
  id: string;
  type: "code" | "document" | "html" | "diagram";
  title: string;
  content: string;
  language?: string;
  messageId?: string;
  versions: Array<{ content: string; ts: number }>;
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

  artifacts: Map<string, ArtifactData> = new Map();
  activeArtifactId: string | null = null;

  private _getGateway: () => GatewayLike | null;
  private _streamStartTime = 0;
  private _streamTimer = 0;
  private _completingTimer = 0;
  private _messageQueue: Array<{
    text: string;
    sessionKey: string;
    attachments?: Array<{ name: string; type: string; data: string }>;
    mentionedFiles?: string[];
    options?: { thinkingEnabled?: boolean };
  }> = [];

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

  async loadEarlier(): Promise<void> {
    this.loadingEarlier = true;
    this._requestUpdate();
    try {
      const gw = (this.host as unknown as { _gw?: GatewayClient })._gw;
      if (gw) {
        const sessionKey =
          this.items.length > 0
            ? ((this.items[0] as Record<string, unknown>).session as string | undefined)
            : undefined;
        const before =
          this.items.length > 0
            ? ((this.items[0] as Record<string, unknown>).id as string | undefined)
            : undefined;
        const res = await gw.request<{ messages?: ChatItem[] }>("chat.history", {
          session: sessionKey,
          before,
          limit: 50,
        });
        const msgs = Array.isArray(res?.messages) ? res.messages : [];
        if (msgs.length > 0) {
          this.items = [...msgs, ...this.items];
          this.trimmedCount = Math.max(0, this.trimmedCount - msgs.length);
        }
        if (msgs.length === 0) this.trimmedCount = 0;
      }
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn("Failed to load earlier messages:", e);
    } finally {
      this.loadingEarlier = false;
      this._requestUpdate();
    }
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
    mentionedFiles?: string[],
    options?: { thinkingEnabled?: boolean },
  ): Promise<void> {
    const gw = this._getGateway();
    if (!gw || gw.status === "disconnected") {
      /* Queue message for delivery when connection is restored */
      this._messageQueue.push({ text, sessionKey, attachments, mentionedFiles, options });
      const userMsg: ChatItem = {
        type: "message",
        role: "user",
        content: text,
        ts: Date.now(),
        status: "failed",
      };
      this.items = [...this.items, userMsg];
      this._trimIfNeeded();
      this.lastFailedMessage = text;
      this.cacheMessages(sessionKey);
      this._requestUpdate();
      return;
    }

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
        ...(mentionedFiles?.length ? { mentionedFiles } : {}),
        ...(options?.thinkingEnabled != null ? { thinkingEnabled: options.thinkingEnabled } : {}),
      });
      this._setLastUserStatus("sent");
    } catch {
      this.isWaiting = false;
      this._stopStreamTimer();
      this._setLastUserStatus("failed");
      this.lastFailedMessage = text;
      this._requestUpdate();
    }
  }

  /** Flush queued messages after reconnection. */
  async flushQueue(): Promise<void> {
    const queued = this._messageQueue.splice(0);
    for (const entry of queued) {
      await this.send(
        entry.text,
        entry.sessionKey,
        entry.attachments,
        entry.mentionedFiles,
        entry.options,
      );
    }
  }

  async abort(): Promise<void> {
    const gw = this._getGateway();
    if (gw) {
      try {
        await gw.abort();
      } catch (e) {
        log.warn("[chat-controller] abort failed:", e);
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
      const item = this.items[failedIdx];
      if (item.type === "message") {
        this.items = [
          ...this.items.slice(0, failedIdx),
          { ...item, status: "sending" as MessageStatus },
          ...this.items.slice(failedIdx + 1),
        ];
      }
    }
    this.lastFailedMessage = "";
    this.isWaiting = true;
    this._startStreamTimer();
    this._requestUpdate();

    const gw = this._getGateway();
    if (!gw) return;
    try {
      await gw.request("chat.send", { message: msg, sessionKey });
      this._setLastUserStatus("sent");
    } catch {
      this.isWaiting = false;
      this._stopStreamTimer();
      this._setLastUserStatus("failed");
      this.lastFailedMessage = msg;
      this._requestUpdate();
    }
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
      return;
    }

    if (event === "artifact.create" || event === "artifact.update") {
      this._handleArtifact(payload, sessionKey);
      return;
    }

    if (event === "memory.recall" || event === "memory.store" || event === "memory.forget") {
      this._handleMemory(event, payload, sessionKey);
      return;
    }

    if (event === "web_search" || event === "web_search.result") {
      this._handleWebSearch(payload, sessionKey);
      return;
    }

    /* Derive enrichment events from agent.tool when the server does not
     * emit dedicated memory / web_search / artifact event names. */
    if (event === "agent.tool") {
      const toolName = (payload.message as string) ?? (payload.name as string) ?? "";
      const state = (payload.state as string) ?? "";
      if (
        toolName === "memory_recall" ||
        toolName === "memory_store" ||
        toolName === "memory_forget"
      ) {
        const action = toolName.replace("memory_", "") as "recall" | "store" | "forget";
        this._handleMemory(`memory.${action}`, payload, sessionKey);
        return;
      }
      if (toolName === "web_search" && state === "result") {
        this._handleWebSearch(payload, sessionKey);
        return;
      }
      if (toolName === "canvas" && state === "result") {
        this._handleArtifact(payload, sessionKey);
        return;
      }
      this._handleToolCall(payload, sessionKey);
      return;
    }
  }

  private _handleMemory(event: string, payload: Record<string, unknown>, sessionKey: string): void {
    const action = event.split(".")[1] as "recall" | "store" | "forget";
    const key = (payload.key as string) ?? "";
    const value = payload.value as string | undefined;
    const item: ChatItem = { type: "memory", action, key, value, ts: Date.now() };
    this.items = [...this.items, item];
    this._trimIfNeeded();
    this.cacheMessages(sessionKey);
    this._requestUpdate();
  }

  private _handleWebSearch(payload: Record<string, unknown>, sessionKey: string): void {
    const query = (payload.query as string) ?? "";
    const sites = (payload.sites as string[]) ?? [];
    const sources = (payload.sources as Array<{ title: string; url: string }>) ?? [];
    const item: ChatItem = { type: "web_search", query, sites, sources, ts: Date.now() };
    this.items = [...this.items, item];
    this._trimIfNeeded();
    this.cacheMessages(sessionKey);
    this._requestUpdate();
  }

  openArtifact(id: string): void {
    if (this.artifacts.has(id)) {
      this.activeArtifactId = id;
      this._requestUpdate();
    }
  }

  closeArtifact(): void {
    this.activeArtifactId = null;
    this._requestUpdate();
  }

  get activeArtifact(): ArtifactData | null {
    if (!this.activeArtifactId) return null;
    return this.artifacts.get(this.activeArtifactId) ?? null;
  }

  private _handleArtifact(payload: Record<string, unknown>, _sessionKey: string): void {
    const id = (payload.id as string) ?? `artifact-${Date.now()}`;
    const type = ((payload.type as string) ?? "code") as ArtifactData["type"];
    const title = (payload.title as string) ?? "Untitled";
    const content = (payload.content as string) ?? "";
    const language = (payload.language as string) ?? "";
    const messageId = payload.message_id as string | undefined;

    const existing = this.artifacts.get(id);
    if (existing) {
      existing.versions.push({ content, ts: Date.now() });
      existing.content = content;
      existing.title = title;
      if (messageId != null) existing.messageId = messageId;
      this.artifacts.set(id, { ...existing });
    } else {
      const lastAssistantId = this._findLastAssistantMessageId();
      this.artifacts.set(id, {
        id,
        type,
        title,
        content,
        language,
        messageId: messageId ?? lastAssistantId,
        versions: [{ content, ts: Date.now() }],
      });
    }
    this.activeArtifactId = id;
    this._requestUpdate();
  }

  private _findLastAssistantMessageId(): string | undefined {
    for (let i = this.items.length - 1; i >= 0; i--) {
      const item = this.items[i];
      if (item.type === "message" && (item as { role: string }).role === "assistant" && item.id) {
        return item.id;
      }
    }
    return undefined;
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
    if (this._completingTimer) {
      window.clearTimeout(this._completingTimer);
      this._completingTimer = 0;
    }
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

    if (state === "thinking") {
      this._handleThinking(payload, sessionKey);
      return;
    }

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

  private _findLastRunningToolCallIndex(toolName?: string): number {
    for (let i = this.items.length - 1; i >= 0; i--) {
      const item = this.items[i];
      if (item.type !== "tool_call" || item.status !== "running") continue;
      if (toolName != null && toolName !== "" && item.name !== toolName) continue;
      return i;
    }
    return -1;
  }

  private _handleToolCall(payload: Record<string, unknown>, sessionKey: string): void {
    const state = payload.state as string | undefined;
    const idFromPayload = payload.id as string | undefined;
    const nameFromMessage = (payload.message as string) ?? "tool";
    const input =
      typeof payload.input === "string"
        ? payload.input
        : payload.args != null
          ? JSON.stringify(payload.args)
          : undefined;

    const hasExplicitResultField = payload.result != null;
    const isResultEvent = state === "result" || (state !== "start" && hasExplicitResultField);
    const resultText = isResultEvent
      ? hasExplicitResultField
        ? String(payload.result)
        : nameFromMessage || undefined
      : undefined;

    if (isResultEvent) {
      let idx = idFromPayload
        ? this.items.findIndex(
            (i): i is Extract<ChatItem, { type: "tool_call" }> =>
              i.type === "tool_call" && i.id === idFromPayload,
          )
        : -1;
      if (idx < 0) {
        idx = this._findLastRunningToolCallIndex(nameFromMessage);
      }
      if (idx >= 0) {
        const existing = this.items[idx];
        if (existing.type === "tool_call") {
          this.items = [
            ...this.items.slice(0, idx),
            {
              ...existing,
              input: existing.input ?? input,
              status: "completed" as const,
              result: resultText ?? existing.result,
            },
            ...this.items.slice(idx + 1),
          ];
        }
      } else {
        const id = idFromPayload ?? `tool-${Date.now()}`;
        this.items = [
          ...this.items,
          {
            type: "tool_call",
            id,
            name: nameFromMessage,
            input,
            status: "completed",
            result: resultText,
            ts: Date.now(),
          },
        ];
        this._trimIfNeeded();
      }
      this.cacheMessages(sessionKey);
      this._requestUpdate();
      return;
    }

    /* state === "start" or legacy start (no state, no result field) */
    const id = idFromPayload ?? `tool-${Date.now()}`;
    const name = nameFromMessage;
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
          status: "running",
          ts: Date.now(),
        },
      ];
      this._trimIfNeeded();
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
