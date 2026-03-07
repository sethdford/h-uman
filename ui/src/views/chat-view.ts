import { html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import type { ContextMenuItem } from "../components/sc-context-menu.js";
import type { ChatSession } from "../components/sc-chat-sessions-panel.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import { ChatController, type ChatItem, type GatewayLike } from "../controllers/chat-controller.js";
import "../components/sc-chat-composer.js";
import "../components/sc-message-thread.js";
import "../components/sc-tapback-menu.js";
import "../components/sc-chat-search.js";
import "../components/sc-chat-sessions-panel.js";
import "../components/sc-context-menu.js";

@customElement("sc-chat-view")
export class ScChatView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      height: 100%;
      max-height: calc(100vh - 120px);
    }
    .main-wrap {
      display: flex;
      flex-direction: row;
      flex: 1;
      min-width: 0;
      position: relative;
      width: 100%;
    }
    .container {
      display: flex;
      flex-direction: column;
      flex: 1;
      height: 100%;
      max-width: 720px;
      margin: 0 auto;
      position: relative;
      width: 100%;
    }
    .status-bar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-xs) var(--sc-space-md);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      background: color-mix(in srgb, var(--sc-bg-surface) 60%, transparent);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--sc-border-subtle);
    }
    .status-left, .status-right {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .status-title {
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
    }
    .kbd-hint {
      display: inline-flex;
      align-items: center;
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      font-size: var(--sc-text-2xs, 10px);
      font-family: var(--sc-font);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      line-height: 1;
    }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; }
    .status-dot.connected { background: var(--sc-success); }
    .status-dot.connecting {
      background: var(--sc-warning);
      animation: sc-pulse var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }
    .status-dot.disconnected { background: var(--sc-text-muted); }
    .retry-btn {
      background: transparent;
      border: 1px solid var(--sc-border);
      color: var(--sc-text-muted);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      cursor: pointer;
      margin-top: var(--sc-space-xs);
      transition: color var(--sc-duration-fast), border-color var(--sc-duration-fast);
    }
    .retry-btn:hover { color: var(--sc-text); border-color: var(--sc-text-muted); }
    .error-banner {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-md);
      background: var(--sc-error-dim);
      border: 1px solid var(--sc-error);
      border-radius: var(--sc-radius);
      color: var(--sc-error);
      font-size: var(--sc-text-base);
    }
    .error-banner button {
      background: none; border: none; color: inherit; cursor: pointer;
      display: flex; align-items: center; padding: 4px;
    }
    .error-banner button svg { width: 16px; height: 16px; line-height: 1; }
    .sessions-toggle {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      background: transparent;
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition: color var(--sc-duration-fast), border-color var(--sc-duration-fast);
    }
    .sessions-toggle:hover { color: var(--sc-text); border-color: var(--sc-text-muted); }
    .sessions-toggle svg { width: 18px; height: 18px; }
    @media (prefers-reduced-motion: reduce) {
      .status-dot.connecting { animation: none !important; }
    }
  `;

  @property() sessionKey = "default";
  private chat = new ChatController(this, () => this.gateway as GatewayLike | null);

  @state() private inputValue = "";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private _searchOpen = false;
  @state() private _searchQuery = "";
  @state() private _searchCurrentMatch = 0;
  @state() private _contextMenu: {
    open: boolean; x: number; y: number; items: ContextMenuItem[];
  } = { open: false, x: 0, y: 0, items: [] };
  @state() private _sessionsPanelOpen = false;
  @state() private _sessions: ChatSession[] = [];
  @state() private _tapback = { open: false, x: 0, y: 0, index: -1, content: "" };
  @query("sc-message-thread") private _messageThread!: HTMLElement & {
    scrollToBottom: () => void;
    scrollToItem: (idx: number) => void;
  };

  private messageHandler = (e: Event) => this.onGatewayMessage(e);
  private statusHandler = (e: Event) => {
    this.connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
  };

  private _handleKeyDown = (e: KeyboardEvent): void => {
    if ((e.metaKey || e.ctrlKey) && e.key === "f") {
      e.preventDefault();
      this._searchOpen = !this._searchOpen;
    }
  };

  private _getSearchMatchIndices(): number[] {
    const q = this._searchQuery.trim().toLowerCase();
    if (!q) return [];
    const indices: number[] = [];
    this.chat.items.forEach((item, idx) => {
      if (item.type === "message" && item.content.toLowerCase().includes(q)) indices.push(idx);
    });
    return indices;
  }

  private _scrollToMatch(matchIndex: number): void {
    const indices = this._getSearchMatchIndices();
    if (matchIndex < 0 || matchIndex >= indices.length) return;
    this._messageThread?.scrollToItem(indices[matchIndex]);
  }

  override firstUpdated(): void {
    const gw = this.gateway;
    if (gw) {
      this.connectionStatus = gw.status;
      gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
      gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    }
    document.addEventListener("keydown", this._handleKeyDown);
  }

  protected override async load(): Promise<void> {
    await this.chat.loadHistory(this.sessionKey);
    this._loadSessions();
  }

  private async _loadSessions(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    try {
      const res = await gw.request<{ sessions?: Array<{ id: string; title: string; ts: number }> }>("sessions.list", {});
      if (res?.sessions && Array.isArray(res.sessions)) {
        this._sessions = res.sessions.map((s) => ({
          id: s.id, title: s.title ?? "Untitled", ts: s.ts ?? Date.now(), active: s.id === this.sessionKey,
        }));
      }
    } catch { this._sessions = []; }
  }

  private async handleAbort(): Promise<void> { await this.chat.abort(); }

  override disconnectedCallback(): void {
    document.removeEventListener("keydown", this._handleKeyDown);
    const gw = this.gateway;
    gw?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    gw?.removeEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    super.disconnectedCallback();
  }

  private onGatewayMessage(e: Event): void {
    const ev = e as CustomEvent;
    const detail = ev.detail as { event?: string; payload?: Record<string, unknown> };
    if (!detail?.event) return;
    const payload = detail.payload ?? {};
    if (detail.event === "health") { this.requestUpdate(); return; }
    this.chat.handleEvent(detail.event, payload, this.sessionKey);
    this._messageThread?.scrollToBottom();
  }

  private async _retry(): Promise<void> {
    try { await this.chat.retry(this.sessionKey); }
    catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this._messageThread?.scrollToBottom();
  }

  private _copyMessage(item: Extract<ChatItem, { type: "message" }>): void {
    navigator.clipboard?.writeText(item.content).catch(() => {});
    ScToast.show({ message: "Copied to clipboard", variant: "success" });
  }

  private _retryMessage(item: Extract<ChatItem, { type: "message" }>): void {
    if (item.role !== "user") return;
    this._handleSend(item.content);
  }

  private _onMessageContextMenu(e: MouseEvent, item: ChatItem): void {
    e.preventDefault();
    if (item.type !== "message") return;
    this._contextMenu = {
      open: true, x: e.clientX, y: e.clientY,
      items: [
        { label: "Copy message", icon: icons.copy, action: () => this._copyMessage(item) },
        { label: "Retry", icon: icons["arrow-clockwise"], action: () => this._retryMessage(item), disabled: item.role === "assistant" },
      ],
    };
  }

  private _handleRegenerate(idx: number): void {
    const items = this.chat.items;
    if (idx < 0 || idx >= items.length) return;
    const target = items[idx];
    if (target.type !== "message" || target.role !== "assistant") return;
    let lastUserIdx = -1;
    for (let i = idx - 1; i >= 0; i--) {
      if (items[i].type === "message" && (items[i] as { role: string }).role === "user") { lastUserIdx = i; break; }
    }
    if (lastUserIdx < 0) return;
    const lastUser = items[lastUserIdx];
    if (lastUser.type !== "message") return;
    this.chat.items = items.slice(0, idx);
    this.chat.cacheMessages(this.sessionKey);
    this._handleSend(lastUser.content);
  }

  private async _handleSend(
    message: string,
    files?: Array<{ name: string; size: number; type: string; dataUrl?: string }>,
  ): Promise<void> {
    if (!message || !this.gateway) return;
    this.inputValue = "";
    const attachments: Array<{ name: string; type: string; data: string }> = [];
    if (files?.length) {
      for (const f of files) {
        this.chat.items = [...this.chat.items, { type: "message", role: "user", content: `\u{1F4CE} ${f.name} (${(f.size / 1024).toFixed(1)} KB)`, ts: Date.now() }];
        if (f.dataUrl) attachments.push({ name: f.name, type: f.type, data: f.dataUrl });
      }
      this.chat.cacheMessages(this.sessionKey);
      this.requestUpdate();
    }
    try {
      await this.chat.send(message, this.sessionKey, attachments.length > 0 ? attachments : undefined);
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this._messageThread?.scrollToBottom();
  }

  private _onSessionSelect(e: CustomEvent<{ id: string }>): void {
    this.dispatchEvent(new CustomEvent("navigate", { bubbles: true, composed: true, detail: `chat:${e.detail.id}` }));
  }
  private _onSessionNew(): void {
    this.dispatchEvent(new CustomEvent("navigate", { bubbles: true, composed: true, detail: "chat:default" }));
  }
  private _onSessionDelete(e: CustomEvent<{ id: string }>): void {
    this._sessions = this._sessions.filter((s) => s.id !== e.detail.id);
    if (this.sessionKey === e.detail.id)
      this.dispatchEvent(new CustomEvent("navigate", { bubbles: true, composed: true, detail: "chat:default" }));
  }
  private async _onSessionRename(e: CustomEvent<{ id: string; title: string }>): Promise<void> {
    const { id, title } = e.detail;
    const gw = this.gateway;
    if (gw) { try { await gw.request("sessions.patch", { key: id, label: title }); this._sessions = this._sessions.map((s) => (s.id === id ? { ...s, title } : s)); } catch {} }
  }

  private _handleSlashCommand(command: string): void {
    if (command === "/export") this._handleExport();
    else if (command === "/clear") { this.chat.items = []; this.chat.cacheMessages(this.sessionKey); }
  }

  private _handleExport(): void {
    const md = this.chat.exportAsMarkdown?.() ?? "";
    if (!md) return;
    const blob = new Blob([md], { type: "text/markdown" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `chat-${this.sessionKey}-${new Date().toISOString().slice(0, 10)}.md`;
    a.click();
    URL.revokeObjectURL(url);
    ScToast.show({ message: "Conversation exported", variant: "success" });
  }

  override render() {
    const sessionsWithActive = this._sessions.map((s) => ({ ...s, active: s.id === this.sessionKey }));
    return html`
      <div class="main-wrap">
        <sc-chat-sessions-panel
          .sessions=${sessionsWithActive}
          ?open=${this._sessionsPanelOpen}
          @sc-session-select=${this._onSessionSelect}
          @sc-session-new=${this._onSessionNew}
          @sc-session-delete=${this._onSessionDelete}
          @sc-session-rename=${this._onSessionRename}
        ></sc-chat-sessions-panel>
        <div class="container">
          ${this._renderStatusBar()} ${this._renderErrorBanner()} ${this._renderSearch()}
          <sc-message-thread
            .items=${this.chat.items}
            .isWaiting=${this.chat.isWaiting}
            .streamElapsed=${this.chat.streamElapsed}
            .historyLoading=${this.chat.historyLoading}
            @sc-context-menu=${(e: CustomEvent<{ event: MouseEvent; item: ChatItem }>) =>
              this._onMessageContextMenu(e.detail.event, e.detail.item)}
            @sc-abort=${() => this.handleAbort()}
            @sc-branch-navigate=${(e: CustomEvent<{ messageId: string; direction: string }>) =>
              this.chat.getBranchMessages?.(e.detail.messageId)}
            @sc-toggle-reaction=${(e: CustomEvent<{ index: number; emoji: string }>) =>
              this.chat.toggleReaction?.(e.detail.index, e.detail.emoji)}
          ></sc-message-thread>
          ${this._renderRetryButton()}
          <sc-chat-composer
            .value=${this.inputValue}
            .waiting=${this.chat.isWaiting}
            .disabled=${this.connectionStatus === "disconnected"}
            .showSuggestions=${this.chat.items.length === 0}
            .streamElapsed=${this.chat.streamElapsed}
            .placeholder=${this.connectionStatus === "disconnected"
              ? "Disconnected \u2014 reconnect to send messages"
              : "Type a message... (Enter to send, Shift+Enter for newline)"}
            @sc-send=${(e: CustomEvent<{ message: string; files?: Array<{ name: string; size: number; type: string; dataUrl?: string }> }>) =>
              this._handleSend(e.detail.message, e.detail.files)}
            @sc-use-suggestion=${(e: CustomEvent<{ text: string }>) => this._handleSend(e.detail.text)}
            @sc-input-change=${(e: CustomEvent<{ value: string }>) => { this.inputValue = e.detail.value; }}
            @sc-abort=${() => this.handleAbort()}
            @sc-slash-command=${(e: CustomEvent<{ command: string }>) => this._handleSlashCommand(e.detail.command)}
          ></sc-chat-composer>
          ${this._contextMenu.open ? html`
            <sc-context-menu .open=${this._contextMenu.open} .x=${this._contextMenu.x} .y=${this._contextMenu.y}
              .items=${this._contextMenu.items} @close=${() => (this._contextMenu = { ...this._contextMenu, open: false })}
            ></sc-context-menu>` : nothing}
          <sc-tapback-menu
            .open=${this._tapback.open} .x=${this._tapback.x} .y=${this._tapback.y}
            .messageIndex=${this._tapback.index} .messageContent=${this._tapback.content}
            @sc-react=${(e: CustomEvent<{ emoji: string; index: number }>) => {
              this.chat.toggleReaction?.(e.detail.index, e.detail.emoji);
              this._tapback = { ...this._tapback, open: false };
            }}
            @sc-tapback-close=${() => (this._tapback = { ...this._tapback, open: false })}
          ></sc-tapback-menu>
        </div>
      </div>
    `;
  }

  private _renderStatusBar() {
    const label = this.connectionStatus === "connected" ? "Connected"
      : this.connectionStatus === "connecting" ? "Reconnecting\u2026" : "Disconnected";
    return html`
      <div class="status-bar">
        <div class="status-left">
          <button type="button" class="sessions-toggle"
            @click=${() => (this._sessionsPanelOpen = !this._sessionsPanelOpen)}
            aria-label=${this._sessionsPanelOpen ? "Close sessions" : "Open sessions"}>
            ${icons["sidebar-toggle"]}
          </button>
          <span class="status-dot ${this.connectionStatus}" aria-hidden="true"></span>
          <span>${label}</span>
        </div>
        <span class="status-title">${this.sessionKey === "default" ? "New Chat" : this.sessionKey}</span>
        <div class="status-right">
          <button type="button" class="sessions-toggle" @click=${() => this._handleExport()} aria-label="Export conversation">
            ${icons.export}
          </button>
          <kbd class="kbd-hint">\u2318F</kbd>
        </div>
      </div>
    `;
  }

  private _renderErrorBanner() {
    if (!this.chat.errorBanner) return nothing;
    return html`
      <div class="error-banner">
        <span>${this.chat.errorBanner}</span>
        <button class="dismiss-btn" @click=${() => (this.chat.errorBanner = "")} aria-label="Dismiss">${icons.x}</button>
      </div>
    `;
  }

  private _renderSearch() {
    if (!this._searchOpen) return nothing;
    return html`
      <sc-chat-search .open=${this._searchOpen} .query=${this._searchQuery}
        .matchCount=${this._getSearchMatchIndices().length} .currentMatch=${this._searchCurrentMatch}
        @sc-search-change=${(e: CustomEvent<{ query: string }>) => { this._searchQuery = e.detail.query; this._searchCurrentMatch = 0; }}
        @sc-search-next=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch = (this._searchCurrentMatch % indices.length) + 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @sc-search-prev=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch = this._searchCurrentMatch <= 1 ? indices.length : this._searchCurrentMatch - 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @sc-search-close=${() => { this._searchOpen = false; this._searchQuery = ""; this._searchCurrentMatch = 0; }}
      ></sc-chat-search>
    `;
  }

  private _renderRetryButton() {
    if (!this.chat.lastFailedMessage) return nothing;
    return html`<button class="retry-btn" @click=${this._retry} aria-label="Retry last message">Retry last message</button>`;
  }
}
