import { html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import type { TemplateResult } from "lit";
import type { GatewayStatus } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { EVENT_NAMES } from "../utils.js";

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

/** Renders basic markdown: bold, italic, code blocks, inline code, links. */
function renderMarkdown(text: string): (TemplateResult | string)[] {
  const parts: (TemplateResult | string)[] = [];
  let remaining = text;
  while (remaining.length > 0) {
    const codeBlock = remaining.match(/^```([\s\S]*?)```/);
    const inlineCode = remaining.match(/^`([^`]+)`/);
    const link = remaining.match(/^\[([^\]]+)\]\(([^)]+)\)/);
    const bold = remaining.match(/^\*\*([^*]+)\*\*/);
    const italic = remaining.match(/^\*([^*]+)\*/);
    const br = remaining.match(/^\n/);
    if (codeBlock) {
      parts.push(html`<pre><code>${codeBlock[1]}</code></pre>`);
      remaining = remaining.slice(codeBlock[0].length);
    } else if (inlineCode) {
      parts.push(html`<code>${inlineCode[1]}</code>`);
      remaining = remaining.slice(inlineCode[0].length);
    } else if (link) {
      parts.push(
        html`<a href="${link[2]}" target="_blank" rel="noopener noreferrer">${link[1]}</a>`,
      );
      remaining = remaining.slice(link[0].length);
    } else if (bold) {
      parts.push(html`<strong>${bold[1]}</strong>`);
      remaining = remaining.slice(bold[0].length);
    } else if (italic) {
      parts.push(html`<em>${italic[1]}</em>`);
      remaining = remaining.slice(italic[0].length);
    } else if (br) {
      parts.push(html`<br />`);
      remaining = remaining.slice(1);
    } else {
      const nextSpecial = remaining.search(/(?:\n|```|`|\[|\*\*|\*)/);
      const chunk = nextSpecial >= 0 ? remaining.slice(0, nextSpecial) : remaining;
      if (chunk) parts.push(chunk);
      remaining = nextSpecial >= 0 ? remaining.slice(nextSpecial) : "";
    }
  }
  return parts;
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
      max-width: 768px;
      margin: 0 auto;
      width: 100%;
    }
    .status-bar {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      padding: 0.5rem 1rem;
      font-size: 0.75rem;
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
      animation: pulse 1s ease-in-out infinite;
    }
    .status-dot.disconnected {
      background: var(--sc-text-muted);
    }
    @keyframes pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.4;
      }
    }
    .messages {
      flex: 1;
      overflow-y: auto;
      padding: 1rem;
      display: flex;
      flex-direction: column;
      gap: 1rem;
    }
    .message {
      max-width: 85%;
      padding: 0.75rem 1rem;
      border-radius: var(--sc-radius);
      font-size: 0.9375rem;
      line-height: 1.5;
      display: flex;
      flex-direction: column;
      gap: 0.25rem;
      animation: sc-slide-up var(--sc-duration-normal, 200ms) var(--sc-ease-out, ease) both;
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
      font-size: 0.6875rem;
      opacity: 0.8;
    }
    .message.user .message-meta {
      align-self: flex-end;
    }
    .message.assistant .message-meta {
      align-self: flex-start;
    }
    .message pre {
      background: var(--sc-bg);
      padding: 0.5rem;
      border-radius: 4px;
      overflow-x: auto;
      margin: 0.5rem 0;
      font-family: var(--sc-font-mono);
      font-size: 0.8125rem;
    }
    .message code {
      font-family: var(--sc-font-mono);
      font-size: 0.875em;
      background: var(--sc-bg-elevated);
      padding: 0.15rem 0.35rem;
      border-radius: 4px;
    }
    .message pre code {
      background: none;
      padding: 0;
    }
    .message a {
      color: var(--sc-accent);
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
      gap: 0.5rem;
      padding: 0.5rem 0.75rem;
      cursor: pointer;
      font-size: 0.875rem;
      user-select: none;
    }
    .tool-header:hover {
      background: var(--sc-bg-surface);
    }
    .tool-header .tool-name {
      font-weight: 600;
      color: var(--sc-accent);
    }
    .tool-expand {
      margin-left: auto;
      font-size: 0.75rem;
      color: var(--sc-text-muted);
    }
    .tool-spinner {
      width: 14px;
      height: 14px;
      border: 2px solid var(--sc-border);
      border-top-color: var(--sc-accent);
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }
    @keyframes spin {
      to {
        transform: rotate(360deg);
      }
    }
    .tool-body {
      padding: 0.75rem 1rem;
      border-top: 1px solid var(--sc-border);
      font-size: 0.8125rem;
      font-family: var(--sc-font-mono);
      color: var(--sc-text-muted);
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 12rem;
      overflow-y: auto;
    }
    .tool-body .label {
      font-size: 0.6875rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: 0.25rem;
      color: var(--sc-text-muted);
    }
    .input-wrap {
      display: flex;
      flex-direction: column;
      gap: 0.25rem;
      padding: 1rem;
      background: var(--sc-bg-surface);
      border-top: 1px solid var(--sc-border);
    }
    .input-bar {
      display: flex;
      gap: 0.5rem;
      align-items: flex-end;
    }
    .input-bar textarea {
      flex: 1;
      min-height: 44px;
      max-height: 120px;
      padding: 0.625rem 1rem;
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: 0.9375rem;
      resize: none;
      line-height: 1.5;
    }
    .input-bar textarea:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle, rgba(249, 112, 102, 0.12));
    }
    .input-bar textarea::placeholder {
      color: var(--sc-text-muted);
    }
    .char-count {
      font-size: 0.6875rem;
      color: var(--sc-text-muted);
    }
    .send-btn {
      padding: 0.625rem 1.25rem;
      min-height: 44px;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-weight: 500;
      cursor: pointer;
      font-size: 0.875rem;
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
      padding: 0.5rem 1rem;
      font-size: 0.875rem;
      color: var(--sc-text-muted);
      font-style: italic;
    }
    .abort-btn {
      margin-left: 0.75rem;
      padding: 0.25rem 0.75rem;
      background: var(--sc-error-dim);
      color: var(--sc-error);
      border: 1px solid var(--sc-error);
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: 0.75rem;
      font-style: normal;
    }
    .abort-btn:hover {
      background: var(--sc-error-dim);
      border-color: var(--sc-error);
    }
    .typing-dots {
      display: inline-flex;
      gap: 4px;
      align-items: center;
      margin-left: 4px;
    }
    .typing-dots span {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background: var(--sc-text-muted);
      animation: sc-bounce-dot 1.4s infinite ease-in-out both;
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
      padding: 0.75rem 1rem;
      background: var(--sc-error-dim);
      border: 1px solid var(--sc-error);
      border-radius: var(--sc-radius);
      color: var(--sc-error);
      font-size: 0.875rem;
    }
    .error-banner button {
      background: none;
      border: none;
      color: inherit;
      cursor: pointer;
      font-size: 1.25rem;
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
  `;

  @property() sessionKey = "default";

  @state() private messages: ChatMessage[] = [];
  @state() private toolCalls: ToolCall[] = [];
  @state() private expandedTools = new Set<string>();
  @state() private inputValue = "";
  @state() private isWaiting = false;
  @state() private errorBanner = "";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @query("#message-list") private messageList!: HTMLElement;
  @query("#chat-input") private inputEl!: HTMLTextAreaElement;

  private messageHandler = (e: Event) => this.onGatewayMessage(e);
  private statusHandler = (e: Event) => {
    this.connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
  };

  override firstUpdated(): void {
    const gw = this.gateway;
    if (!gw) return;
    this.connectionStatus = gw.status;
    gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.messageHandler);
    gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
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
        this.scrollToBottom();
      }
    } catch {
      /* history load is best-effort */
    }
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
        const last = this.messages[this.messages.length - 1];
        if (!last || last.role !== "user" || last.content !== content) {
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

  private toggleTool(id: string): void {
    const next = new Set(this.expandedTools);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    this.expandedTools = next;
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

  private async send(): Promise<void> {
    const text = this.inputValue.trim();
    if (!text || !this.gateway) return;
    this.messages = [...this.messages, { role: "user", content: text, ts: Date.now() }];
    this.inputValue = "";
    this.isWaiting = true;
    this.resizeTextarea();
    this.scrollToBottom();
    try {
      await this.gateway.request("chat.send", {
        message: text,
        sessionKey: this.sessionKey,
      });
    } catch (err) {
      this.isWaiting = false;
      this.errorBanner = err instanceof Error ? err.message : "Failed to send message";
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
          <span class="status-dot ${this.connectionStatus}"></span>
          <span>${statusLabel}</span>
        </div>
        ${this.errorBanner
          ? html`
              <div class="error-banner">
                <span>${this.errorBanner}</span>
                <button @click=${() => (this.errorBanner = "")} aria-label="Dismiss">×</button>
              </div>
            `
          : nothing}
        <div id="message-list" class="messages">
          ${this.messages.length === 0 && this.toolCalls.length === 0
            ? html`
                <div
                  style="display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;color:var(--sc-text-muted);gap:var(--sc-space-md);text-align:center;padding:var(--sc-space-2xl);"
                >
                  <svg
                    width="48"
                    height="48"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="1.5"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    style="opacity:0.4"
                  >
                    <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z" />
                  </svg>
                  <div>
                    <p
                      style="margin:0;font-size:var(--sc-text-lg);font-weight:var(--sc-weight-semibold);color:var(--sc-text)"
                    >
                      Start a conversation
                    </p>
                    <p style="margin:var(--sc-space-xs) 0 0;font-size:var(--sc-text-sm)">
                      Type a message below to begin chatting with SeaClaw.
                    </p>
                  </div>
                </div>
              `
            : nothing}
          ${this.messages.map(
            (m) => html`
              <div class="message ${m.role}">
                ${m.role === "assistant" ? renderMarkdown(m.content) : m.content}
                ${m.ts != null
                  ? html`<span class="message-meta">${formatTime(m.ts)}</span>`
                  : nothing}
              </div>
            `,
          )}
          ${this.toolCalls.map(
            (t) => html`
              <div class="tool-card">
                <div
                  class="tool-header"
                  @click=${() => this.toggleTool(t.id)}
                  role="button"
                  tabindex="0"
                  @keydown=${(e: KeyboardEvent) => {
                    if (e.key === "Enter" || e.key === " ") {
                      e.preventDefault();
                      this.toggleTool(t.id);
                    }
                  }}
                >
                  ${t.status === "running" ? html`<span class="tool-spinner"></span>` : nothing}
                  <span class="tool-name">Tool: ${t.name}</span>
                  <span class="tool-expand"> ${this.expandedTools.has(t.id) ? "▼" : "▶"} </span>
                </div>
                ${this.expandedTools.has(t.id)
                  ? html`
                      <div class="tool-body">
                        ${t.input != null
                          ? html`
                              <div class="label">Input</div>
                              <div>${t.input}</div>
                            `
                          : nothing}
                        ${t.result != null
                          ? html`
                              <div class="label" style="margin-top: 0.5rem">Output</div>
                              <div>${t.result}</div>
                            `
                          : t.status === "running"
                            ? html`<div class="label">Executing…</div>`
                            : nothing}
                      </div>
                    `
                  : nothing}
              </div>
            `,
          )}
          ${this.isWaiting
            ? html`<div class="thinking">
                Thinking<span class="typing-dots"><span></span><span></span><span></span></span
                ><button class="abort-btn" @click=${() => this.handleAbort()}>Abort</button>
              </div>`
            : nothing}
        </div>
        <div class="input-wrap">
          <div class="input-bar">
            <textarea
              id="chat-input"
              placeholder="Type a message... (Enter to send, Shift+Enter for newline)"
              .value=${this.inputValue}
              @input=${this.handleInput}
              @keydown=${this.handleKeyDown}
            ></textarea>
            <button
              class="send-btn"
              ?disabled=${!this.inputValue.trim() || this.isWaiting}
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
