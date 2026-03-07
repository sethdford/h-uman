import { html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import type { ContextMenuItem } from "../components/sc-context-menu.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { formatTime } from "../utils.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import { ChatController, type ChatItem, type GatewayLike } from "../controllers/chat-controller.js";
import "../components/sc-empty-state.js";
import "../components/sc-thinking.js";
import "../components/sc-tool-result.js";
import "../components/sc-message-stream.js";
import "../components/sc-reasoning-block.js";
import "../components/sc-chat-search.js";
import "../components/sc-context-menu.js";
import "../components/sc-skeleton.js";

@customElement("sc-chat-view")
export class ScChatView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      height: 100%;
      max-height: calc(100vh - 120px);
    }
    .container {
      display: flex;
      flex-direction: column;
      height: 100%;
      max-width: 720px;
      margin: 0 auto;
      position: relative;
      width: 100%;
    }
    .status-bar {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      background: var(--sc-bg-surface);
      border-bottom: 1px solid var(--sc-border);
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
    }
    .status-dot.connected {
      background: var(--sc-success);
    }
    .status-dot.connecting {
      background: var(--sc-warning);
      animation: sc-pulse var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }
    .status-dot.disconnected {
      background: var(--sc-text-muted);
    }
    .messages {
      flex: 1;
      overflow-y: auto;
      padding: var(--sc-space-md);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
    }
    .messages.drag-over {
      outline: 2px dashed var(--sc-accent);
      outline-offset: -4px;
      background: color-mix(in srgb, var(--sc-accent) 4%, transparent);
    }
    .message {
      max-width: 85%;
      padding: var(--sc-space-md) var(--sc-space-md);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-base);
      line-height: 1.5;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      animation: sc-slide-up var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .message.user {
      align-self: flex-end;
      background: var(--sc-accent);
      color: var(--sc-bg);
    }
    .message.assistant {
      align-self: flex-start;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      color: var(--sc-text);
    }
    .message-meta {
      font-size: var(--sc-text-xs);
      opacity: var(--sc-opacity-muted, 0.8);
    }
    .message.user .message-meta {
      align-self: flex-end;
    }
    .message.assistant .message-meta {
      align-self: flex-start;
    }
    .message code {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      background: var(--sc-bg-elevated);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      border-radius: var(--sc-radius-sm);
    }
    .suggested-prompts {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
      justify-content: center;
      margin-top: var(--sc-space-md);
    }
    .prompt-pill {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-full);
      padding: var(--sc-space-xs) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      cursor: pointer;
      transition: all var(--sc-duration-fast) var(--sc-ease-out);
      white-space: nowrap;
    }
    .prompt-pill:hover {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      color: var(--sc-accent);
    }
    .prompt-pill:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .scroll-bottom-pill {
      position: absolute;
      bottom: 90px;
      left: 50%;
      transform: translateX(-50%);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      box-shadow: var(--sc-shadow-md);
      padding: var(--sc-space-xs) var(--sc-space-md);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text);
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      z-index: 5;
      animation: sc-fade-up var(--sc-duration-fast) var(--sc-ease-out);
    }
    .scroll-bottom-pill:hover {
      background: var(--sc-bg-elevated);
    }
    .pill-icon svg {
      width: 14px;
      height: 14px;
      vertical-align: -2px;
    }
    @keyframes sc-fade-up {
      from {
        opacity: 0;
        transform: translateX(-50%) translateY(8px);
      }
      to {
        opacity: 1;
        transform: translateX(-50%) translateY(0);
      }
    }
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
      transition:
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast);
    }
    .retry-btn:hover {
      color: var(--sc-text);
      border-color: var(--sc-text-muted);
    }
    .message a {
      color: var(--sc-accent-text, var(--sc-accent));
      text-decoration: underline;
    }
    .message a:hover {
      color: var(--sc-accent-hover);
    }
    .input-wrap {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-md);
      background: var(--sc-bg-surface);
      border-top: 1px solid var(--sc-border);
    }
    .input-bar {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: flex-end;
    }
    .input-bar textarea {
      flex: 1;
      min-height: 44px;
      max-height: 120px;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      resize: none;
      line-height: 1.5;
    }
    .input-bar textarea:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    .input-bar textarea::placeholder {
      color: var(--sc-text-muted);
    }
    .char-count {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .send-btn {
      padding: var(--sc-space-sm) var(--sc-space-lg);
      min-height: 44px;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      font-size: var(--sc-text-base);
    }
    .send-btn:hover:not(:disabled) {
      background: var(--sc-accent-hover);
    }
    .send-btn:disabled {
      opacity: var(--sc-opacity-disabled, 0.5);
      cursor: not-allowed;
    }
    .thinking {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      align-self: flex-start;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      font-style: italic;
    }
    .stream-elapsed {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
    }
    .abort-btn {
      margin-left: var(--sc-space-md);
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: var(--sc-error-dim);
      color: var(--sc-error);
      border: 1px solid var(--sc-error);
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: var(--sc-text-xs);
      font-style: normal;
    }
    .abort-btn:hover {
      background: var(--sc-error-dim);
      border-color: var(--sc-error);
    }
    .typing-dots {
      display: inline-flex;
      gap: var(--sc-space-xs);
      align-items: center;
      margin-left: var(--sc-space-xs);
    }
    .typing-dots span {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background: var(--sc-text-muted);
      animation: sc-bounce-dot var(--sc-duration-slow) var(--sc-ease-in-out) infinite both;
    }
    .typing-dots span:nth-child(1) {
      animation-delay: -0.32s;
    }
    .typing-dots span:nth-child(2) {
      animation-delay: -0.16s;
    }
    @keyframes sc-bounce-dot {
      0%,
      80%,
      100% {
        transform: scale(0);
      }
      40% {
        transform: scale(1);
      }
    }
    .error-banner {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-md) var(--sc-space-md);
      background: var(--sc-error-dim);
      border: 1px solid var(--sc-error);
      border-radius: var(--sc-radius);
      color: var(--sc-error);
      font-size: var(--sc-text-base);
    }
    .error-banner button {
      background: none;
      border: none;
      color: inherit;
      cursor: pointer;
      display: flex;
      align-items: center;
      padding: 4px;
    }
    .error-banner button svg {
      width: 16px;
      height: 16px;
      line-height: 1;
    }
    @media (max-width: 640px) {
      .message {
        max-width: 95%;
      }
      .input-bar {
        flex-direction: column;
        align-items: stretch;
      }
      .send-btn {
        min-height: 40px;
      }
    }
    .history-skeleton {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      padding: var(--sc-space-lg) 0;
    }
    @media (prefers-reduced-motion: reduce) {
      .status-dot.connecting,
      .message,
      .scroll-bottom-pill,
      .typing-dots span {
        animation: none !important;
      }
    }
  `;

  @property() sessionKey = "default";

  private chat = new ChatController(this, () => this.gateway as GatewayLike | null);

  @state() private inputValue = "";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private showScrollPill = false;
  @state() private _searchOpen = false;
  @state() private _searchQuery = "";
  @state() private _searchCurrentMatch = 0;
  @state() private _dragOver = false;
  @state() private _contextMenu: {
    open: boolean;
    x: number;
    y: number;
    items: ContextMenuItem[];
  } = { open: false, x: 0, y: 0, items: [] };
  @query("#message-list") private messageList!: HTMLElement;
  @query("#chat-input") private inputEl!: HTMLTextAreaElement;

  private messageHandler = (e: Event) => this.onGatewayMessage(e);
  private statusHandler = (e: Event) => {
    this.connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
  };

  private _scrollHandler = () => {
    const el = this.messageList;
    if (!el) return;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 60;
    if (this.showScrollPill === atBottom) this.showScrollPill = !atBottom;
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
      if (item.type === "message" && item.content.toLowerCase().includes(q)) {
        indices.push(idx);
      }
    });
    return indices;
  }

  private _scrollToMatch(matchIndex: number): void {
    const indices = this._getSearchMatchIndices();
    if (matchIndex < 0 || matchIndex >= indices.length) return;
    const itemIdx = indices[matchIndex];
    this.updateComplete.then(() => {
      const msgEl = this.messageList?.querySelector(`#msg-${itemIdx}`) as HTMLElement;
      msgEl?.scrollIntoView({ block: "nearest", behavior: "smooth" });
    });
  }

  private _handleDragOver(e: DragEvent): void {
    e.preventDefault();
    this._dragOver = true;
  }

  private _handleDragLeave(): void {
    this._dragOver = false;
  }

  private _handleDrop(e: DragEvent): void {
    e.preventDefault();
    this._dragOver = false;
    const files = Array.from(e.dataTransfer?.files ?? []);
    if (files.length > 0) {
      this._handleFiles(files);
    }
  }

  private _handleFiles(files: File[]): void {
    for (const file of files) {
      this.chat.items = [
        ...this.chat.items,
        {
          type: "message",
          role: "user",
          content: `[Attached file: ${file.name} (${(file.size / 1024).toFixed(1)} KB)]`,
          ts: Date.now(),
        },
      ];
    }
    this.chat.cacheMessages(this.sessionKey);
    this.requestUpdate();
    this.scrollToBottom();
  }

  override firstUpdated(): void {
    const gw = this.gateway;
    if (gw) {
      this.connectionStatus = gw.status;
      gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
      gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    }
    this.messageList?.addEventListener("scroll", this._scrollHandler, { passive: true });
    document.addEventListener("keydown", this._handleKeyDown);
  }

  protected override async load(): Promise<void> {
    await this.chat.loadHistory(this.sessionKey);
  }

  private async handleAbort(): Promise<void> {
    await this.chat.abort();
  }

  override disconnectedCallback(): void {
    document.removeEventListener("keydown", this._handleKeyDown);
    const gw = this.gateway;
    gw?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    gw?.removeEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    this.messageList?.removeEventListener("scroll", this._scrollHandler);
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
    this.scrollToBottom();
  }

  private _findLastAssistantIdx(): number {
    for (let i = this.chat.items.length - 1; i >= 0; i--) {
      if (
        this.chat.items[i].type === "message" &&
        (this.chat.items[i] as { role: string }).role === "assistant"
      ) {
        return i;
      }
    }
    return -1;
  }

  private scrollToBottom(): void {
    this.updateComplete.then(() => {
      const el = this.messageList;
      if (el) el.scrollTop = el.scrollHeight;
    });
  }

  private resizeTextarea(): void {
    const el = this.inputEl;
    if (!el) return;
    el.style.height = "auto";
    const lineHeight = parseInt(getComputedStyle(el).lineHeight, 10) || 24;
    const maxHeight = lineHeight * 5;
    el.style.height = `${Math.min(el.scrollHeight, maxHeight)}px`;
  }

  private _useSuggestion(text: string): void {
    this.inputValue = text;
    this.requestUpdate();
    this.updateComplete.then(() => {
      this.inputEl?.focus();
    });
  }

  private async _retry(): Promise<void> {
    try {
      await this.chat.retry(this.sessionKey);
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this.scrollToBottom();
  }

  private _copyMessage(item: Extract<ChatItem, { type: "message" }>): void {
    navigator.clipboard?.writeText(item.content).catch(() => {});
    ScToast.show({ message: "Copied to clipboard", variant: "success" });
  }

  private _retryMessage(item: Extract<ChatItem, { type: "message" }>): void {
    if (item.role !== "user") return;
    this.inputValue = item.content;
    this.send();
  }

  private _onMessageContextMenu(e: MouseEvent, item: ChatItem): void {
    e.preventDefault();
    if (item.type !== "message") return;
    this._contextMenu = {
      open: true,
      x: e.clientX,
      y: e.clientY,
      items: [
        {
          label: "Copy message",
          icon: icons.copy,
          action: () => this._copyMessage(item),
        },
        {
          label: "Retry",
          icon: icons["arrow-clockwise"],
          action: () => this._retryMessage(item),
          disabled: item.role === "assistant",
        },
      ],
    };
  }

  private async send(): Promise<void> {
    const text = this.inputValue.trim();
    if (!text || !this.gateway) return;
    this.inputValue = "";
    this.resizeTextarea();
    try {
      await this.chat.send(text, this.sessionKey);
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
    this.scrollToBottom();
  }

  private handleKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this.send();
    }
  }

  private handleInput(): void {
    this.inputValue = (this.inputEl?.value ?? "") as string;
    this.resizeTextarea();
  }

  override render() {
    return html`
      <div class="container">
        ${this._renderStatusBar()} ${this._renderErrorBanner()}
        <div
          id="message-list"
          class="messages ${this._dragOver ? "drag-over" : ""}"
          role="log"
          aria-live="polite"
          aria-label="${this._dragOver ? "Drop files to attach" : "Chat messages"}"
          aria-dropeffect=${this._dragOver ? "copy" : "none"}
          @dragover=${this._handleDragOver}
          @dragleave=${this._handleDragLeave}
          @drop=${this._handleDrop}
        >
          ${this._renderSearch()}
          ${this.chat.items.length === 0 ? this._renderEmptyState() : nothing}
          ${this._renderMessages()} ${this.chat.isWaiting ? this._renderThinking() : nothing}
        </div>
        ${this._renderScrollPill()} ${this._renderRetryButton()} ${this._renderInputBar()}
        ${this._contextMenu.open
          ? html`
              <sc-context-menu
                .open=${this._contextMenu.open}
                .x=${this._contextMenu.x}
                .y=${this._contextMenu.y}
                .items=${this._contextMenu.items}
                @close=${() => (this._contextMenu = { ...this._contextMenu, open: false })}
              ></sc-context-menu>
            `
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
        <span class="status-dot ${this.connectionStatus}" aria-hidden="true"></span>
        <span>${label}</span>
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

  private _renderSearch() {
    if (!this._searchOpen) return nothing;
    return html`
      <sc-chat-search
        .open=${this._searchOpen}
        .query=${this._searchQuery}
        .matchCount=${this._getSearchMatchIndices().length}
        .currentMatch=${this._searchCurrentMatch}
        @sc-search-change=${(e: CustomEvent<{ query: string }>) => {
          this._searchQuery = e.detail.query;
          this._searchCurrentMatch = 0;
        }}
        @sc-search-next=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch = (this._searchCurrentMatch % indices.length) + 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @sc-search-prev=${() => {
          const indices = this._getSearchMatchIndices();
          if (indices.length === 0) return;
          this._searchCurrentMatch =
            this._searchCurrentMatch <= 1 ? indices.length : this._searchCurrentMatch - 1;
          this._scrollToMatch(this._searchCurrentMatch - 1);
        }}
        @sc-search-close=${() => {
          this._searchOpen = false;
          this._searchQuery = "";
          this._searchCurrentMatch = 0;
        }}
      ></sc-chat-search>
    `;
  }

  private _renderEmptyState() {
    return html`
      <sc-empty-state
        .icon=${icons["chat-circle"]}
        heading="Start a conversation"
        description="Ask SeaClaw anything — write code, answer questions, use tools."
      >
        <div class="suggested-prompts">
          <button
            class="prompt-pill"
            @click=${() => this._useSuggestion("Explain how this project is architected")}
          >
            Explain how this project is architected
          </button>
          <button
            class="prompt-pill"
            @click=${() => this._useSuggestion("Write a Python web scraper")}
          >
            Write a Python web scraper
          </button>
          <button class="prompt-pill" @click=${() => this._useSuggestion("Help me debug an issue")}>
            Help me debug an issue
          </button>
          <button class="prompt-pill" @click=${() => this._useSuggestion("What can you do?")}>
            What can you do?
          </button>
        </div>
      </sc-empty-state>
    `;
  }

  private _renderMessages() {
    if (this.chat.historyLoading) {
      return html`
        <div class="history-skeleton">
          <sc-skeleton variant="line" width="60%"></sc-skeleton>
          <sc-skeleton variant="line" width="80%"></sc-skeleton>
          <sc-skeleton variant="line" width="45%"></sc-skeleton>
        </div>
      `;
    }
    const lastAssistantIdx = this._findLastAssistantIdx();
    return this.chat.items.map((item, idx) => {
      if (item.type === "message") {
        const isStreaming =
          this.chat.isWaiting && item.role === "assistant" && idx === lastAssistantIdx;
        return html`
          <div
            id="msg-${idx}"
            class="message ${item.role}"
            style="--sc-stagger-index: ${idx}; animation-delay: min(calc(var(--sc-stagger-delay) * var(--sc-stagger-index)), var(--sc-stagger-max));"
            @contextmenu=${(ev: MouseEvent) => this._onMessageContextMenu(ev, item)}
          >
            <sc-message-stream
              .content=${item.content}
              .streaming=${isStreaming}
              .role=${item.role}
            ></sc-message-stream>
            ${item.ts != null
              ? html`<span class="message-meta">${formatTime(item.ts)}</span>`
              : nothing}
          </div>
        `;
      }
      if (item.type === "tool_call") {
        return html`
          <sc-tool-result
            .tool=${item.name}
            .status=${item.status === "completed"
              ? item.result?.startsWith("Error")
                ? "error"
                : "success"
              : "running"}
            .content=${item.result ?? item.input ?? ""}
          ></sc-tool-result>
        `;
      }
      if (item.type === "thinking") {
        return html`
          <sc-reasoning-block
            .content=${item.content}
            .streaming=${item.streaming}
            .duration=${item.duration ?? ""}
          ></sc-reasoning-block>
        `;
      }
      return nothing;
    });
  }

  private _renderThinking() {
    return html`
      <div class="thinking">
        <sc-thinking .active=${true} .steps=${[]}></sc-thinking>
        <span class="stream-elapsed">${this.chat.streamElapsed}</span>
        <button class="abort-btn" @click=${() => this.handleAbort()} aria-label="Stop generating">
          Abort
        </button>
      </div>
    `;
  }

  private _renderScrollPill() {
    if (!this.showScrollPill) return nothing;
    return html`
      <button
        class="scroll-bottom-pill"
        @click=${() => this.scrollToBottom()}
        aria-label="Scroll to latest messages"
      >
        <span class="pill-icon">${icons["arrow-down"]}</span> New messages
      </button>
    `;
  }

  private _renderRetryButton() {
    if (!this.chat.lastFailedMessage) return nothing;
    return html`<button class="retry-btn" @click=${this._retry} aria-label="Retry last message">
      Retry last message
    </button>`;
  }

  private _renderInputBar() {
    return html`
      <div class="input-wrap">
        <div class="input-bar">
          <textarea
            id="chat-input"
            placeholder=${this.connectionStatus === "disconnected"
              ? "Disconnected \u2014 reconnect to send messages"
              : "Type a message... (Enter to send, Shift+Enter for newline)"}
            .value=${this.inputValue}
            ?disabled=${this.connectionStatus === "disconnected"}
            @input=${this.handleInput}
            @keydown=${this.handleKeyDown}
          ></textarea>
          <button
            class="send-btn"
            ?disabled=${!this.inputValue.trim() ||
            this.chat.isWaiting ||
            this.connectionStatus === "disconnected"}
            @click=${() => this.send()}
            aria-label="Send"
          >
            Send
          </button>
        </div>
        <span class="char-count">${this.inputValue.length} characters</span>
      </div>
    `;
  }
}
