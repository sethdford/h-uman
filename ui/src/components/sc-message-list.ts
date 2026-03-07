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
const FIVE_MIN_MS = 5 * 60 * 1000;

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
    }
    @keyframes sc-slide-up {
      from {
        opacity: 0;
        transform: translateY(12px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    .message-row {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: flex-end;
    }
    .message-row.user-row {
      justify-content: flex-end;
    }
    .message-row.assistant-row {
      justify-content: flex-start;
    }
    .message-row.sender-change {
      margin-top: var(--sc-space-lg);
    }
    .message-row.same-sender {
      margin-top: var(--sc-space-2xs);
    }
    .message-row:first-child {
      margin-top: 0;
    }
    .avatar {
      flex-shrink: 0;
      width: var(--sc-icon-xl);
      height: var(--sc-icon-xl);
      min-width: var(--sc-icon-xl);
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font);
    }
    .avatar.assistant {
      background: var(--sc-accent);
      color: var(--sc-on-accent);
    }
    .avatar.user {
      background: var(--sc-bg-elevated);
      color: var(--sc-text-muted);
    }
    .avatar-spacer {
      flex-shrink: 0;
      width: var(--sc-icon-xl);
      min-width: var(--sc-icon-xl);
    }
    .time-divider {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: var(--sc-space-sm);
      margin: var(--sc-space-md) 0;
    }
    .time-divider::before,
    .time-divider::after {
      content: "";
      flex: 1;
      height: 1px;
      background: linear-gradient(to right, transparent, var(--sc-border-subtle));
    }
    .time-divider::after {
      background: linear-gradient(to left, transparent, var(--sc-border-subtle));
    }
    .time-divider span {
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-faint);
      white-space: nowrap;
    }
    .message {
      position: relative;
      max-width: 75%;
      padding: var(--sc-space-md) var(--sc-space-md);
      font-size: var(--sc-text-base);
      line-height: 1.5;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      animation: sc-slide-up var(--sc-duration-fast) var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .message:hover sc-message-actions {
      opacity: 1;
      transform: translateY(0);
    }
    @media (hover: none) {
      .message sc-message-actions {
        opacity: var(--sc-opacity-overlay-heavy);
        transform: translateY(0);
      }
    }
    .message.user {
      align-self: flex-end;
      background: var(
        --sc-user-message-gradient,
        linear-gradient(135deg, var(--sc-accent-strong), var(--sc-accent))
      );
      color: var(--sc-on-accent);
      border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm) var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-xs);
    }
    .message.assistant {
      align-self: flex-start;
      max-width: 85%;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm);
      box-shadow: var(--sc-shadow-xs);
      color: var(--sc-text);
    }
    .message.continuation {
      margin-top: 0;
    }
    .message.continuation .message-meta {
      display: none;
    }
    .message-meta {
      font-size: var(--sc-text-xs);
      opacity: 0;
      transform: translateY(-4px);
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
    }
    .message:hover .message-meta {
      opacity: 0.8;
      transform: translateY(0);
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
      bottom: 90px; /* sc-lint-ok: scroll-to-bottom offset above composer */
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
      background: transparent;
      color: var(--sc-text-muted);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      font-style: normal;
      transition:
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast),
        background var(--sc-duration-fast);
    }
    .abort-btn:hover {
      color: var(--sc-error);
      border-color: var(--sc-error);
      background: var(--sc-error-dim);
    }
    .history-skeleton {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      padding: var(--sc-space-lg) 0;
    }
    .skeleton-row {
      display: flex;
    }
    .skeleton-right {
      justify-content: flex-end;
    }
    .skeleton-left {
      justify-content: flex-start;
    }
    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      .message.user {
        max-width: 95%;
      }
      .message.assistant {
        max-width: 95%;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .messages {
        scroll-behavior: auto;
      }
      .message,
      .scroll-bottom-pill {
        animation: none !important;
      }
      .message-meta {
        transition: none;
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
      const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
      el?.scrollIntoView({ block: "nearest", behavior: reduceMotion ? "auto" : "smooth" });
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

  private _renderMessageRow(
    item: Extract<ChatItem, { type: "message" }>,
    idx: number,
  ): ReturnType<typeof html>[] {
    const lastAssistantIdx = this._findLastAssistantIdx();
    const isStreaming = this.isWaiting && item.role === "assistant" && idx === lastAssistantIdx;
    const isContinuation = this._computeContinuation(idx);
    const prevItem = this.items[idx - 1];
    const prevMsg = prevItem?.type === "message" ? prevItem : null;
    const prevTs = prevMsg?.ts ?? 0;
    const ts = item.ts ?? 0;
    const needsDivider = !isContinuation && prevMsg != null && Math.abs(ts - prevTs) > FIVE_MIN_MS;
    const senderChange = prevMsg == null || prevMsg.role !== item.role;
    const rowMarginClass = idx === 0 ? "" : senderChange ? "sender-change" : "same-sender";

    const parts: ReturnType<typeof html>[] = [];
    if (needsDivider) {
      parts.push(html` <div class="time-divider"><span>${formatTime(ts)}</span></div> `);
    }

    const staggerDelay = `min(${idx * 30}ms, 300ms)`;
    const avatarEl = isContinuation
      ? html`<span class="avatar-spacer" aria-hidden="true"></span>`
      : item.role === "assistant"
        ? html`<span class="avatar assistant" aria-hidden="true">~</span>`
        : html`<span class="avatar user" aria-hidden="true">U</span>`;

    parts.push(html`
      <div class="message-row ${item.role}-row ${rowMarginClass}" id="msg-row-${idx}">
        ${avatarEl}
        <div
          id="msg-${idx}"
          class="message ${item.role} ${isContinuation ? "continuation" : ""}"
          style="animation-delay: ${staggerDelay};"
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
      </div>
    `);

    return parts;
  }

  private _renderItem(
    item: ChatItem,
    idx: number,
  ): ReturnType<typeof html> | ReturnType<typeof html>[] {
    if (item.type === "message") {
      return this._renderMessageRow(item, idx);
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
                <div class="skeleton-row skeleton-right">
                  <sc-skeleton
                    variant="line"
                    width="55%"
                    height="40px"
                    style="border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm) var(--sc-radius-lg); overflow: hidden;"
                  ></sc-skeleton>
                </div>
                <div class="skeleton-row skeleton-left">
                  <sc-skeleton
                    variant="line"
                    width="70%"
                    height="56px"
                    style="border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm); overflow: hidden;"
                  ></sc-skeleton>
                </div>
                <div class="skeleton-row skeleton-right">
                  <sc-skeleton
                    variant="line"
                    width="45%"
                    height="36px"
                    style="border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm) var(--sc-radius-lg); overflow: hidden;"
                  ></sc-skeleton>
                </div>
                <div class="skeleton-row skeleton-left">
                  <sc-skeleton
                    variant="line"
                    width="65%"
                    height="48px"
                    style="border-radius: var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-lg) var(--sc-radius-sm); overflow: hidden;"
                  ></sc-skeleton>
                </div>
              </div>
            `
          : html`
              ${this.items.flatMap((item, idx) => {
                const result = this._renderItem(item, idx);
                return Array.isArray(result) ? result : [result];
              })}
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
