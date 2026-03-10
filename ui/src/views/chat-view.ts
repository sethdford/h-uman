import { html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import type { ContextMenuItem } from "../components/hu-context-menu.js";
import type { ChatSession } from "../components/hu-chat-sessions-panel.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import { ChatController, type ChatItem, type GatewayLike } from "../controllers/chat-controller.js";
import "../components/hu-button.js";
import "../components/hu-chat-composer.js";
import "../components/hu-message-thread.js";
import "../components/hu-tapback-menu.js";
import "../components/hu-chat-search.js";
import "../components/hu-chat-sessions-panel.js";
import "../components/hu-context-menu.js";
import "../components/hu-status-dot.js";

@customElement("hu-chat-view")
export class ScChatView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-chat;
      display: flex;
      flex-direction: column;
      height: 100%;
      max-height: calc(100vh - var(--hu-space-5xl));
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
      max-width: 45rem;
      margin: 0 auto;
      position: relative;
      width: 100%;
    }
    .status-bar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-xs) var(--hu-space-md);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      background: color-mix(in srgb, var(--hu-bg-surface) 60%, transparent);
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      border-bottom: 1px solid var(--hu-border-subtle);
    }
    .status-left,
    .status-right {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }
    .status-title {
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
      font-size: var(--hu-text-sm);
    }
    .kbd-hint {
      display: inline-flex;
      align-items: center;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      font-size: var(--hu-text-2xs);
      font-family: var(--hu-font);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      line-height: 1;
    }
    .retry-btn {
      margin-top: var(--hu-space-xs);
    }
    .error-banner {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-md);
      background: var(--hu-error-dim);
      border: 1px solid var(--hu-error);
      border-radius: var(--hu-radius);
      color: var(--hu-error);
      font-size: var(--hu-text-base);
    }
    .error-banner button {
      background: none;
      border: none;
      color: inherit;
      cursor: pointer;
      display: flex;
      align-items: center;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
    }
    .error-banner button svg {
      width: 1rem;
      height: 1rem;
      line-height: 1;
    }
    .sessions-toggle {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: transparent;
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast),
        border-color var(--hu-duration-fast);
    }
    .sessions-toggle:hover {
      color: var(--hu-text);
      border-color: var(--hu-text-muted);
    }
    .sessions-toggle svg {
      width: 1.125rem;
      height: 1.125rem;
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
    open: boolean;
    x: number;
    y: number;
    items: ContextMenuItem[];
  } = { open: false, x: 0, y: 0, items: [] };
  @state() private _sessionsPanelOpen = false;
  @state() private _sessions: ChatSession[] = [];
  @state() private _tapback = { open: false, x: 0, y: 0, index: -1, content: "" };
  @state() private _sessionsLoading = false;
  @query("hu-chat-composer") private _composer!: HTMLElement & { focus?: () => void };
  @query("hu-message-thread") private _messageThread!: HTMLElement & {
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

  protected override onGatewaySwapped(
    previous: GatewayClientClass | null,
    current: GatewayClientClass,
  ): void {
    previous?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    previous?.removeEventListener(
      GatewayClientClass.EVENT_STATUS,
      this.statusHandler as EventListener,
    );
    current.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    current.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    this.connectionStatus = current.status;
  }

  protected override async load(): Promise<void> {
    await this.chat.loadHistory(this.sessionKey);
    await this._loadSessions();
  }

  private async _loadSessions(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this._sessionsLoading = true;
    try {
      const res = await gw.request<{
        sessions?: Array<{
          key?: string;
          label?: string;
          created_at?: number;
          last_active?: number;
          turn_count?: number;
        }>;
      }>("sessions.list", {});
      if (res?.sessions && Array.isArray(res.sessions)) {
        this._sessions = res.sessions.map((s) => {
          const id = s.key ?? "";
          const title = s.label ?? "Untitled";
          const ts = s.last_active ?? s.created_at ?? Date.now();
          return {
            id,
            title,
            ts: typeof ts === "number" ? ts : Date.now(),
            active: id === this.sessionKey,
          };
        });
      }
    } catch (e) {
      console.warn("Failed to load sessions:", e);
      this._sessions = [];
    } finally {
      this._sessionsLoading = false;
    }
  }

  private async handleAbort(): Promise<void> {
    await this.chat.abort();
  }

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
    if (detail.event === "health") {
      this.requestUpdate();
      return;
    }
    this.chat.handleEvent(detail.event, payload, this.sessionKey);
    this._messageThread?.scrollToBottom();
  }

  private async _retry(): Promise<void> {
    try {
      await this.chat.retry(this.sessionKey);
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this._messageThread?.scrollToBottom();
  }

  private _copyMessage(item: Extract<ChatItem, { type: "message" }>): void {
    navigator.clipboard
      ?.writeText(item.content)
      .then(() => {
        ScToast.show({ message: "Copied to clipboard", variant: "success" });
      })
      .catch(() => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      });
  }

  private _retryMessage(item: Extract<ChatItem, { type: "message" }>): void {
    if (item.role !== "user") return;
    this._handleSend(item.content);
  }

  private _onMessageContextMenu(e: MouseEvent, item: ChatItem): void {
    e.preventDefault();
    if (item.type !== "message") return;
    this._contextMenu = {
      open: true,
      x: e.clientX,
      y: e.clientY,
      items: [
        { label: "Copy message", icon: icons.copy, action: () => this._copyMessage(item) },
        {
          label: "Retry",
          icon: icons["arrow-clockwise"],
          action: () => this._retryMessage(item),
          disabled: item.role === "assistant",
        },
        {
          label: "Add reaction",
          icon: icons.heart,
          action: () => {
            const idx = this.chat.items.findIndex((i) => i === item);
            this._messageThread.dispatchEvent(
              new CustomEvent("hu-tapback", {
                bubbles: true,
                composed: true,
                detail: {
                  x: this._contextMenu.x,
                  y: this._contextMenu.y,
                  index: idx,
                  content: item.type === "message" ? item.content : "",
                },
              }),
            );
          },
        },
      ],
    };
  }

  private _handleEdit(content: string, _index: number): void {
    this.inputValue = content;
    this.requestUpdate();
    this.updateComplete.then(() => this._composer?.focus?.());
  }

  private _handleRegenerate(idx: number): void {
    const items = this.chat.items;
    if (idx < 0 || idx >= items.length) return;
    const target = items[idx];
    if (target.type !== "message" || target.role !== "assistant") return;
    let lastUserIdx = -1;
    for (let i = idx - 1; i >= 0; i--) {
      if (items[i].type === "message" && (items[i] as { role: string }).role === "user") {
        lastUserIdx = i;
        break;
      }
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
    mentionedFiles?: string[],
  ): Promise<void> {
    if (!message || !this.gateway) return;
    this.inputValue = "";
    const attachments: Array<{ name: string; type: string; data: string }> = [];
    if (files?.length) {
      for (const f of files) {
        this.chat.items = [
          ...this.chat.items,
          {
            type: "message",
            role: "user",
            content: `[attachment] ${f.name} (${(f.size / 1024).toFixed(1)} KB)`,
            ts: Date.now(),
          },
        ];
        if (f.dataUrl) attachments.push({ name: f.name, type: f.type, data: f.dataUrl });
      }
      this.chat.cacheMessages(this.sessionKey);
      this.requestUpdate();
    }
    try {
      await this.chat.send(
        message,
        this.sessionKey,
        attachments.length > 0 ? attachments : undefined,
        mentionedFiles,
      );
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this._messageThread?.scrollToBottom();
  }

  private _onSessionSelect(e: CustomEvent<{ id: string }>): void {
    this.dispatchEvent(
      new CustomEvent("navigate", { bubbles: true, composed: true, detail: `chat:${e.detail.id}` }),
    );
  }
  private _onSessionNew(): void {
    this.dispatchEvent(
      new CustomEvent("navigate", { bubbles: true, composed: true, detail: "chat:default" }),
    );
  }
  private _onSessionDelete(e: CustomEvent<{ id: string }>): void {
    this._sessions = this._sessions.filter((s) => s.id !== e.detail.id);
    if (this.sessionKey === e.detail.id)
      this.dispatchEvent(
        new CustomEvent("navigate", { bubbles: true, composed: true, detail: "chat:default" }),
      );
  }
  private async _onSessionRename(e: CustomEvent<{ id: string; title: string }>): Promise<void> {
    const { id, title } = e.detail;
    const gw = this.gateway;
    if (!gw) return;
    // Save original label for revert on failure
    const session = this._sessions.find((s) => s.id === id);
    const originalLabel = session?.title ?? "Untitled";
    try {
      // Call server first
      await gw.request("sessions.patch", { key: id, label: title });
      // Update UI on success
      this._sessions = this._sessions.map((s) => (s.id === id ? { ...s, title } : s));
    } catch (e) {
      // Revert the UI update on failure
      this._sessions = this._sessions.map((s) =>
        s.id === id ? { ...s, title: originalLabel } : s,
      );
      this.requestUpdate();
      ScToast.show({ message: "Failed to rename session", variant: "error" });
    }
  }

  private _handleSlashCommand(command: string): void {
    if (command === "/export") this._handleExport();
    else if (command === "/clear") {
      this.chat.items = [];
      this.chat.cacheMessages(this.sessionKey);
    }
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
    const sessionsWithActive = this._sessions.map((s) => ({
      ...s,
      active: s.id === this.sessionKey,
    }));
    return html`
      <div class="main-wrap">
        <hu-chat-sessions-panel
          .sessions=${sessionsWithActive}
          ?open=${this._sessionsPanelOpen}
          @hu-session-select=${this._onSessionSelect}
          @hu-session-new=${this._onSessionNew}
          @hu-session-delete=${this._onSessionDelete}
          @hu-session-rename=${this._onSessionRename}
        ></hu-chat-sessions-panel>
        <div class="container">
          ${this._renderStatusBar()} ${this._renderErrorBanner()}
          ${this._renderHistoryErrorBanner()} ${this._renderSearch()}
          <hu-message-thread
            .items=${this.chat.items}
            .isWaiting=${this.chat.isWaiting}
            .streamElapsed=${this.chat.streamElapsed}
            .historyLoading=${this.chat.historyLoading}
            .hasEarlierMessages=${this.chat.hasEarlierMessages}
            .loadingEarlier=${this.chat.loadingEarlier}
            @hu-context-menu=${(e: CustomEvent<{ event: MouseEvent; item: ChatItem }>) =>
              this._onMessageContextMenu(e.detail.event, e.detail.item)}
            @hu-abort=${() => this.handleAbort()}
            @hu-load-earlier=${() => this.chat.loadEarlier()}
            @hu-branch-navigate=${(e: CustomEvent<{ index: number; direction: number }>) => {
              const item = this.chat.items[e.detail.index];
              if (item?.type === "message" && item.id && item.branchCount && item.branchCount > 1) {
                const newIndex = (item.branchIndex ?? 0) + e.detail.direction;
                if (newIndex >= 0 && newIndex < item.branchCount) {
                  this.chat.items = [
                    ...this.chat.items.slice(0, e.detail.index),
                    { ...item, branchIndex: newIndex },
                    ...this.chat.items.slice(e.detail.index + 1),
                  ];
                  this.requestUpdate();
                }
              }
            }}
            @hu-toggle-reaction=${(e: CustomEvent<{ index: number; value: string }>) =>
              this.chat.toggleReaction?.(e.detail.index, e.detail.value)}
            @hu-swipe-reply=${(e: CustomEvent<{ index: number; content: string }>) => {
              this.inputValue = e.detail.content;
              this.requestUpdate();
              this.updateComplete.then(() => this._composer?.focus?.());
            }}
            @hu-swipe-copy=${(e: CustomEvent<{ index: number; content: string }>) => {
              navigator.clipboard?.writeText(e.detail.content).then(
                () => ScToast.show({ message: "Copied to clipboard", variant: "success" }),
                () => ScToast.show({ message: "Failed to copy", variant: "error" }),
              );
            }}
            @hu-retry=${(e: CustomEvent<{ content?: string; index?: number }>) => {
              if (e.detail?.content != null) {
                const idx = e.detail.index ?? -1;
                const item = idx >= 0 ? this.chat.items[idx] : undefined;
                if (item?.type === "message" && item.role === "user" && item.status === "failed") {
                  this.chat.items = [
                    ...this.chat.items.slice(0, idx),
                    ...this.chat.items.slice(idx + 1),
                  ];
                  this.chat.cacheMessages(this.sessionKey);
                }
                this._handleSend(e.detail.content);
              } else {
                this._retry();
              }
            }}
            @hu-regenerate=${(e: CustomEvent<{ content: string; index: number }>) => {
              this._handleRegenerate(e.detail.index);
            }}
            @hu-edit=${(e: CustomEvent<{ content: string; index: number }>) => {
              this._handleEdit(e.detail.content, e.detail.index);
            }}
            @hu-edit-message=${(e: CustomEvent<{ index: number }>) => {
              const item = this.chat.items[e.detail.index];
              if (item?.type === "message" && item.role === "user") {
                this._handleEdit(item.content, e.detail.index);
              }
            }}
            @hu-reply-message=${(e: CustomEvent<{ content: string }>) => {
              this.inputValue = e.detail.content;
              this.requestUpdate();
              this.updateComplete.then(() => this._composer?.focus?.());
            }}
            @hu-copy-message=${() => {
              ScToast.show({ message: "Copied to clipboard", variant: "success" });
            }}
            @hu-tapback=${(
              e: CustomEvent<{ x: number; y: number; index: number; content: string }>,
            ) => {
              this._tapback = {
                open: true,
                x: e.detail.x,
                y: e.detail.y,
                index: e.detail.index,
                content: e.detail.content,
              };
            }}
            @hu-suggestion-click=${(e: CustomEvent<{ text: string }>) =>
              this._handleSend(e.detail.text)}
            @open-artifact=${async (e: CustomEvent<{ id: string }>) => {
              await import("../components/hu-artifact-panel.js");
              this.chat.openArtifact(e.detail.id);
            }}
            .artifacts=${Array.from(this.chat.artifacts.values())}
          ></hu-message-thread>
          ${this._renderRetryButton()}
          <hu-chat-composer
            .value=${this.inputValue}
            .waiting=${this.chat.isWaiting}
            .disabled=${this.connectionStatus === "disconnected"}
            .showSuggestions=${this.chat.items.length === 0}
            .streamElapsed=${this.chat.streamElapsed}
            .placeholder=${this.connectionStatus === "disconnected"
              ? "Disconnected \u2014 reconnect to send messages"
              : "Type a message... (Enter to send, Shift+Enter for newline)"}
            @hu-send=${(
              e: CustomEvent<{
                message: string;
                files?: Array<{ name: string; size: number; type: string; dataUrl?: string }>;
                mentionedFiles?: string[];
              }>,
            ) => this._handleSend(e.detail.message, e.detail.files, e.detail.mentionedFiles)}
            @hu-use-suggestion=${(e: CustomEvent<{ text: string }>) =>
              this._handleSend(e.detail.text)}
            @hu-input-change=${(e: CustomEvent<{ value: string }>) => {
              this.inputValue = e.detail.value;
            }}
            @hu-abort=${() => this.handleAbort()}
            @hu-slash-command=${(e: CustomEvent<{ command: string }>) =>
              this._handleSlashCommand(e.detail.command)}
          ></hu-chat-composer>
          ${this._contextMenu.open
            ? html` <hu-context-menu
                .open=${this._contextMenu.open}
                .x=${this._contextMenu.x}
                .y=${this._contextMenu.y}
                .items=${this._contextMenu.items}
                @close=${() => (this._contextMenu = { ...this._contextMenu, open: false })}
              ></hu-context-menu>`
            : nothing}
          <hu-tapback-menu
            .open=${this._tapback.open}
            .x=${this._tapback.x}
            .y=${this._tapback.y}
            .messageIndex=${this._tapback.index}
            .messageContent=${this._tapback.content}
            @hu-react=${(e: CustomEvent<{ value: string; index: number }>) => {
              this.chat.toggleReaction?.(e.detail.index, e.detail.value);
              this._tapback = { ...this._tapback, open: false };
            }}
            @hu-tapback-close=${() => (this._tapback = { ...this._tapback, open: false })}
          ></hu-tapback-menu>
        </div>
        ${this.chat.activeArtifact
          ? html`<hu-artifact-panel
              .artifact=${this.chat.activeArtifact}
              .open=${true}
              @hu-artifact-close=${() => this.chat.closeArtifact()}
            ></hu-artifact-panel>`
          : nothing}
      </div>
    `;
  }

  private _renderStatusBar() {
    const label =
      this.connectionStatus === "connected"
        ? "Connected"
        : this.connectionStatus === "connecting"
          ? "Reconnecting\u2026"
          : "Disconnected";
    return html`
      <div class="status-bar">
        <div class="status-left">
          <button
            type="button"
            class="sessions-toggle"
            @click=${() => (this._sessionsPanelOpen = !this._sessionsPanelOpen)}
            aria-label=${this._sessionsPanelOpen ? "Close sessions" : "Open sessions"}
          >
            ${icons["sidebar-toggle"]}
          </button>
          <hu-status-dot status=${this.connectionStatus}></hu-status-dot>
          <span>${label}</span>
        </div>
        <span class="status-title"
          >${this.sessionKey === "default" ? "New Chat" : this.sessionKey}</span
        >
        <div class="status-right">
          <button
            type="button"
            class="sessions-toggle"
            @click=${() => this._handleExport()}
            aria-label="Export conversation"
          >
            ${icons.export}
          </button>
          <kbd class="kbd-hint">⌘F</kbd>
        </div>
      </div>
    `;
  }

  private _renderErrorBanner() {
    if (!this.chat.errorBanner) return nothing;
    return html`
      <div class="error-banner">
        <span>${this.chat.errorBanner}</span>
        <button
          class="dismiss-btn"
          @click=${() => (this.chat.errorBanner = "")}
          aria-label="Dismiss"
        >
          ${icons.x}
        </button>
      </div>
    `;
  }

  private _renderHistoryErrorBanner() {
    if (!this.chat.historyError) return nothing;
    return html`
      <div class="error-banner">
        <span>${this.chat.historyError}</span>
        <hu-button
          variant="ghost"
          size="sm"
          @click=${() => this.chat.loadHistory(this.sessionKey)}
          aria-label="Retry loading history"
        >
          Retry
        </hu-button>
      </div>
    `;
  }

  private _renderSearch() {
    if (!this._searchOpen) return nothing;
    return html`
      <hu-chat-search
        .open=${this._searchOpen}
        .query=${this._searchQuery}
        .matchCount=${this._getSearchMatchIndices().length}
        .currentMatch=${this._searchCurrentMatch}
        @hu-search-change=${(e: CustomEvent<{ query: string }>) => {
          this._searchQuery = e.detail.query;
          this._searchCurrentMatch = 0;
        }}
        @hu-search-next=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch = (this._searchCurrentMatch % indices.length) + 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @hu-search-prev=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch =
            this._searchCurrentMatch <= 1 ? indices.length : this._searchCurrentMatch - 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @hu-search-close=${() => {
          this._searchOpen = false;
          this._searchQuery = "";
          this._searchCurrentMatch = 0;
        }}
      ></hu-chat-search>
    `;
  }

  private _renderRetryButton() {
    if (!this.chat.lastFailedMessage) return nothing;
    return html`<hu-button
      variant="ghost"
      size="sm"
      @click=${this._retry}
      aria-label="Retry last message"
      class="retry-btn"
    >
      Retry last message
    </hu-button>`;
  }
}
