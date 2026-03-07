import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import "./sc-message-stream.js";
import "./sc-message-actions.js";
import "./sc-tool-result.js";
import "./sc-reasoning-block.js";
import "./sc-thinking.js";
import "./sc-skeleton.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import { formatTime } from "../utils.js";

const TWO_MIN_MS = 2 * 60 * 1000;

@customElement("sc-message-list")
export class ScMessageList extends LitElement {
  @property({ type: Array }) items: ChatItem[] = [];

  @property({ type: Boolean }) isWaiting = false;

  @property({ type: String }) streamElapsed = "";

  @property({ type: Boolean }) historyLoading = false;

  @state() private showScrollPill = false;

  @query("#scroll-container") private scrollContainer!: HTMLElement;

  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1;
      position: relative;
      min-height: 0;
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
      position: relative;
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
    .message:hover sc-message-actions {
      opacity: 1;
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
    .message.continuation {
      margin-top: calc(var(--sc-space-xs) - var(--sc-space-md));
    }
    .message.continuation .message-meta {
      display: none;
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
    .message a {
      color: var(--sc-accent-text, var(--sc-accent));
      text-decoration: underline;
    }
    .message a:hover {
      color: var(--sc-accent-hover);
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
    .history-skeleton {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      padding: var(--sc-space-lg) 0;
    }
    @media (max-width: 640px) {
      .message {
        max-width: 95%;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .message,
      .scroll-bottom-pill {
        animation: none !important;
      }
    }
  `;

  private _scrollHandler = (): void => {
    const el = this.scrollContainer;
    if (!el) return;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 60;
    if (this.showScrollPill === atBottom) this.showScrollPill = !atBottom;
  };

  override firstUpdated(): void {
    this.scrollContainer?.addEventListener("scroll", this._scrollHandler, { passive: true });
  }

  override disconnectedCallback(): void {
    this.scrollContainer?.removeEventListener("scroll", this._scrollHandler);
    super.disconnectedCallback();
  }

  scrollToBottom(): void {
    this.updateComplete.then(() => {
      const el = this.scrollContainer;
      if (el) el.scrollTop = el.scrollHeight;
    });
  }

  scrollToItem(idx: number): void {
    this.updateComplete.then(() => {
      const el = this.scrollContainer?.querySelector(`#msg-${idx}`) as HTMLElement;
      el?.scrollIntoView({ block: "nearest", behavior: "smooth" });
    });
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

  private _onContextMenu(ev: MouseEvent, item: ChatItem): void {
    ev.preventDefault();
    this.dispatchEvent(
      new CustomEvent("sc-context-menu", {
        bubbles: true,
        composed: true,
        detail: { event: ev, item },
      }),
    );
  }

  private _computeContinuation(idx: number): boolean {
    const item = this.items[idx];
    if (item.type !== "message") return false;
    if (idx === 0) return false;
    const prev = this.items[idx - 1];
    if (prev.type !== "message") return false;
    if (prev.role !== item.role) return false;
    const ts = item.ts ?? 0;
    const prevTs = prev.ts ?? 0;
    return Math.abs(ts - prevTs) <= TWO_MIN_MS;
  }

  private _onAbort(): void {
    this.dispatchEvent(
      new CustomEvent("sc-abort", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _renderItem(item: ChatItem, idx: number): ReturnType<typeof html> {
    const lastAssistantIdx = this._findLastAssistantIdx();
    if (item.type === "message") {
      const isStreaming = this.isWaiting && item.role === "assistant" && idx === lastAssistantIdx;
      const isContinuation = this._computeContinuation(idx);
      return html`
        <div
          id="msg-${idx}"
          class="message ${item.role} ${isContinuation ? "continuation" : ""}"
          style="--sc-stagger-index: ${idx}; animation-delay: min(calc(var(--sc-stagger-delay) * var(--sc-stagger-index)), var(--sc-stagger-max));"
          @contextmenu=${(ev: MouseEvent) => this._onContextMenu(ev, item)}
        >
          <sc-message-actions
            .role=${item.role}
            .content=${item.content}
            .index=${idx}
          ></sc-message-actions>
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
    return html``;
  }

  override render() {
    return html`
      <div
        id="scroll-container"
        class="messages"
        role="log"
        aria-live="polite"
        aria-label="Chat messages"
      >
        ${this.historyLoading
          ? html`
              <div class="history-skeleton">
                <sc-skeleton variant="line" width="60%"></sc-skeleton>
                <sc-skeleton variant="line" width="80%"></sc-skeleton>
                <sc-skeleton variant="line" width="45%"></sc-skeleton>
              </div>
            `
          : html`
              ${this.items.map((item, idx) => this._renderItem(item, idx))}
              ${this.isWaiting
                ? html`
                    <div class="thinking">
                      <sc-thinking .active=${true} .steps=${[]}></sc-thinking>
                      <span class="stream-elapsed">${this.streamElapsed}</span>
                      <button
                        class="abort-btn"
                        @click=${this._onAbort}
                        aria-label="Stop generating"
                      >
                        Abort
                      </button>
                    </div>
                  `
                : nothing}
            `}
      </div>
      ${this.showScrollPill
        ? html`
            <button
              class="scroll-bottom-pill"
              @click=${() => this.scrollToBottom()}
              aria-label="Scroll to latest messages"
            >
              <span class="pill-icon">${icons["arrow-down"]}</span> New messages
            </button>
          `
        : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-message-list": ScMessageList;
  }
}
