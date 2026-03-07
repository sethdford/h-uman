import { html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { EVENT_NAMES } from "../utils.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-empty-state.js";
import "../components/sc-thinking.js";
import "../components/sc-tool-result.js";
import "../components/sc-message-stream.js";

interface ChatMessage {
  role: "user" | "assistant";
  content: string;
  id?: string;
  ts?: number;
}

interface ToolCall {
  id: string;
  name: string;
  input?: string;
  status: "running" | "completed";
  result?: string;
}

function formatTime(ts: number): string {
  const d = new Date(ts);
  return d.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
  });
}

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
    .message {
      max-width: 85%;
      padding: var(--sc-space-md) var(--sc-space-md);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-base);
      line-height: 1.5;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      animation: sc-slide-up var(--sc-duration-normal) var(--sc-ease-out) both;
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
      opacity: 0.8;
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
    .tool-card {
      align-self: flex-start;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      max-width: 85%;
      overflow: hidden;
    }
    .tool-header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      cursor: pointer;
      font-size: var(--sc-text-base);
      user-select: none;
    }
    .tool-header:hover {
      background: var(--sc-bg-surface);
    }
    .tool-header .tool-name {
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-accent-text, var(--sc-accent));
    }
    .tool-expand {
      margin-left: auto;
      color: var(--sc-text-muted);
    }
    .tool-expand svg {
      width: 12px;
      height: 12px;
      transition: transform var(--sc-duration-normal) var(--sc-ease-out);
    }
    .tool-spinner {
      width: 14px;
      height: 14px;
      border: 2px solid var(--sc-border);
      border-top-color: var(--sc-accent);
      border-radius: 50%;
      animation: sc-spin var(--sc-duration-slow) linear infinite;
    }
    @keyframes sc-spin {
      to {
        transform: rotate(360deg);
      }
    }
    .tool-body {
      padding: var(--sc-space-md) var(--sc-space-md);
      border-top: 1px solid var(--sc-border);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font-mono);
      color: var(--sc-text-muted);
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 12rem;
      overflow-y: auto;
    }
    .tool-body .label {
      font-size: var(--sc-text-xs);
      text-transform: uppercase;
      letter-spacing: 0.02em;
      margin-bottom: var(--sc-space-xs);
      color: var(--sc-text-muted);
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
      opacity: 0.5;
      cursor: not-allowed;
    }
    .thinking {
      align-self: flex-start;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      font-style: italic;
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
      .message,
      .tool-card {
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
    @media (prefers-reduced-motion: reduce) {
      .status-dot.connecting,
      .message,
      .scroll-bottom-pill,
      .tool-spinner,
      .typing-dots span {
        animation: none !important;
      }
    }
  `;

  @property() sessionKey = "default";

  @state() private messages: ChatMessage[] = [];
  @state() private toolCalls: ToolCall[] = [];
  @state() private inputValue = "";
  @state() private isWaiting = false;
  @state() private errorBanner = "";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private showScrollPill = false;
  @state() private lastFailedMessage = "";
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

  override firstUpdated(): void {
    const gw = this.gateway;
    if (!gw) return;
    this.connectionStatus = gw.status;
    gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    this.messageList?.addEventListener("scroll", this._scrollHandler, { passive: true });
  }

  private get _cacheKey(): string {
    return `sc-chat-${this.sessionKey}`;
  }

  private _cacheMessages(): void {
    try {
      sessionStorage.setItem(this._cacheKey, JSON.stringify(this.messages));
    } catch {
      /* quota exceeded — ignore */
    }
  }

  private _restoreFromCache(): boolean {
    try {
      const raw = sessionStorage.getItem(this._cacheKey);
      if (!raw) return false;
      const cached = JSON.parse(raw) as ChatMessage[];
      if (Array.isArray(cached) && cached.length > 0) {
        this.messages = cached;
        this.scrollToBottom();
        return true;
      }
    } catch {
      /* corrupt cache — ignore */
    }
    return false;
  }

  protected override async load(): Promise<void> {
    await this.loadHistory();
  }

  private async loadHistory(): Promise<void> {
    if (!this.gateway) return;
    try {
      const res = await this.gateway.request<{
        messages?: { role: string; content: string }[];
      }>("chat.history", { sessionKey: this.sessionKey });
      if (res?.messages && Array.isArray(res.messages) && res.messages.length > 0) {
        this.messages = res.messages.map((m) => ({
          role: m.role as "user" | "assistant",
          content: m.content ?? "",
        }));
        this._cacheMessages();
        this.scrollToBottom();
        return;
      }
    } catch {
      /* history load is best-effort */
    }
    this._restoreFromCache();
  }

  private async handleAbort(): Promise<void> {
    if (!this.gateway) return;
    try {
      await this.gateway.abort();
    } catch {
      /* abort is best-effort */
    }
    this.isWaiting = false;
  }

  override disconnectedCallback(): void {
    const gw = this.gateway;
    gw?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    gw?.removeEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    this.messageList?.removeEventListener("scroll", this._scrollHandler);
    super.disconnectedCallback();
  }

  private onGatewayMessage(e: Event): void {
    const ev = e as CustomEvent;
    const detail = ev.detail as {
      event?: string;
      payload?: Record<string, unknown>;
    };
    if (!detail?.event) return;
    const payload = detail.payload ?? {};
    if (detail.event === EVENT_NAMES.ERROR) {
      const msg = (payload.message as string) ?? (payload.error as string) ?? "Unknown error";
      this.errorBanner = msg;
      this.requestUpdate();
    }
    if (detail.event === EVENT_NAMES.HEALTH) {
      this.requestUpdate();
    }
    if (detail.event === EVENT_NAMES.CHAT) {
      const state = payload.state as string;
      const content = (payload.message as string) ?? "";
      if (state === "received" && content) {
        const recentUser = this.messages
          .slice(-6)
          .some((m) => m.role === "user" && m.content === content);
        if (!recentUser) {
          this.messages = [
            ...this.messages,
            {
              role: "user",
              content,
              id: payload.id as string,
              ts: Date.now(),
            },
          ];
        }
      }
      if (state === "sent" && content) {
        this.messages = [
          ...this.messages,
          {
            role: "assistant",
            content,
            id: payload.id as string,
            ts: Date.now(),
          },
        ];
        this.isWaiting = false;
      }
      if (state === "chunk" && content) {
        const last = this.messages[this.messages.length - 1];
        if (last && last.role === "assistant") {
          this.messages = [
            ...this.messages.slice(0, -1),
            { ...last, content: last.content + content },
          ];
        } else {
          this.messages = [
            ...this.messages,
            {
              role: "assistant",
              content,
              id: payload.id as string,
              ts: Date.now(),
            },
          ];
        }
      }
      if (state === "sent" && !content) {
        this.isWaiting = true;
      }
      this.requestUpdate();
      this.scrollToBottom();
      this._cacheMessages();
    }
    if (detail.event === EVENT_NAMES.TOOL_CALL) {
      const id = (payload.id as string) ?? `tool-${Date.now()}`;
      const name = (payload.message as string) ?? "tool";
      const input =
        typeof payload.input === "string"
          ? payload.input
          : payload.args != null
            ? JSON.stringify(payload.args)
            : undefined;
      const result = payload.result != null ? String(payload.result) : undefined;
      const existing = this.toolCalls.find((t) => t.id === id);
      if (!existing) {
        this.toolCalls = [
          ...this.toolCalls,
          {
            id,
            name,
            input,
            status: result != null ? "completed" : "running",
            result,
          },
        ];
      } else {
        this.toolCalls = this.toolCalls.map((t) =>
          t.id === id
            ? {
                ...t,
                input: t.input ?? input,
                status: "completed" as const,
                result: result ?? t.result,
              }
            : t,
        );
      }
      this.requestUpdate();
      this.scrollToBottom();
    }
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
    const lineHeight = 24;
    const maxLines = 5;
    const maxHeight = lineHeight * maxLines;
    el.style.height = `${Math.min(el.scrollHeight, maxHeight)}px`;
  }

  private _useSuggestion(text: string): void {
    this.inputValue = text;
    this.requestUpdate();
    this.updateComplete.then(() => {
      this.inputEl?.focus();
    });
  }

  private _retry(): void {
    if (!this.lastFailedMessage) return;
    this.inputValue = this.lastFailedMessage;
    this.lastFailedMessage = "";
    this.send();
  }

  private async send(): Promise<void> {
    const text = this.inputValue.trim();
    if (!text || !this.gateway) return;
    this.messages = [...this.messages, { role: "user", content: text, ts: Date.now() }];
    this.inputValue = "";
    this.lastFailedMessage = "";
    this.isWaiting = true;
    this._cacheMessages();
    this.resizeTextarea();
    this.scrollToBottom();
    try {
      await this.gateway.request("chat.send", {
        message: text,
        sessionKey: this.sessionKey,
      });
    } catch (err) {
      this.isWaiting = false;
      this.lastFailedMessage = text;
      const msg = err instanceof Error ? err.message : "Failed to send message";
      ScToast.show({ message: msg, variant: "error" });
    }
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
    const statusLabel =
      this.connectionStatus === "connected"
        ? "Connected"
        : this.connectionStatus === "connecting"
          ? "Reconnecting…"
          : "Disconnected";

    return html`
      <div class="container">
        <div class="status-bar">
          <span class="status-dot ${this.connectionStatus}" aria-hidden="true"></span>
          <span>${statusLabel}</span>
        </div>
        ${this.errorBanner
          ? html`
              <div class="error-banner">
                <span>${this.errorBanner}</span>
                <button
                  class="dismiss-btn"
                  @click=${() => (this.errorBanner = "")}
                  aria-label="Dismiss"
                >
                  ${icons.x}
                </button>
              </div>
            `
          : nothing}
        <div
          id="message-list"
          class="messages"
          role="log"
          aria-live="polite"
          aria-label="Chat messages"
        >
          ${this.messages.length === 0 && this.toolCalls.length === 0
            ? html`
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
                    <button
                      class="prompt-pill"
                      @click=${() => this._useSuggestion("Help me debug an issue")}
                    >
                      Help me debug an issue
                    </button>
                    <button
                      class="prompt-pill"
                      @click=${() => this._useSuggestion("What can you do?")}
                    >
                      What can you do?
                    </button>
                  </div>
                </sc-empty-state>
              `
            : nothing}
          ${this.messages.map(
            (m, i) => html`
              <div class="message ${m.role}">
                <sc-message-stream
                  .content=${m.content}
                  .streaming=${this.isWaiting &&
                  m.role === "assistant" &&
                  i === this.messages.length - 1}
                  .role=${m.role}
                ></sc-message-stream>
                ${m.ts != null
                  ? html`<span class="message-meta">${formatTime(m.ts)}</span>`
                  : nothing}
              </div>
            `,
          )}
          ${this.toolCalls.map(
            (tc) => html`
              <sc-tool-result
                .tool=${tc.name}
                .status=${tc.status === "completed"
                  ? tc.result?.startsWith("Error")
                    ? "error"
                    : "success"
                  : "running"}
                .content=${tc.result ?? tc.input ?? ""}
              ></sc-tool-result>
            `,
          )}
          ${this.isWaiting
            ? html`
                <div
                  class="thinking"
                  style="display:flex;align-items:center;gap:var(--sc-space-sm);flex-wrap:wrap;"
                >
                  <sc-thinking .active=${true} .steps=${[]}></sc-thinking>
                  <button class="abort-btn" @click=${() => this.handleAbort()}>Abort</button>
                </div>
              `
            : nothing}
        </div>
        ${this.showScrollPill
          ? html`<button class="scroll-bottom-pill" @click=${() => this.scrollToBottom()}>
              <span class="pill-icon">${icons["arrow-down"]}</span> New messages
            </button>`
          : nothing}
        ${this.lastFailedMessage
          ? html`<button class="retry-btn" @click=${this._retry}>Retry last message</button>`
          : nothing}
        <div class="input-wrap">
          <div class="input-bar">
            <textarea
              id="chat-input"
              placeholder=${this.connectionStatus === "disconnected"
                ? "Disconnected — reconnect to send messages"
                : "Type a message... (Enter to send, Shift+Enter for newline)"}
              .value=${this.inputValue}
              ?disabled=${this.connectionStatus === "disconnected"}
              @input=${this.handleInput}
              @keydown=${this.handleKeyDown}
            ></textarea>
            <button
              class="send-btn"
              ?disabled=${!this.inputValue.trim() ||
              this.isWaiting ||
              this.connectionStatus === "disconnected"}
              @click=${() => this.send()}
              aria-label="Send"
            >
              Send
            </button>
          </div>
          <span class="char-count">${this.inputValue.length} characters</span>
        </div>
      </div>
    `;
  }
}
