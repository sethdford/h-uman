import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import "./sc-chat-bubble.js";
import "./sc-message-group.js";
import "./sc-typing-indicator.js";
import "./sc-delivery-status.js";
import "./sc-tool-result.js";
import "./sc-reasoning-block.js";
import "./sc-thinking.js";
import "./sc-skeleton.js";
import "./sc-message-actions.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import { formatTime } from "../utils.js";

const FIVE_MIN_MS = 5 * 60 * 1000;

type Block =
  | { type: "time-divider"; ts: number }
  | {
      type: "message-group";
      role: "user" | "assistant";
      messages: Array<{ item: Extract<ChatItem, { type: "message" }>; idx: number }>;
      lastTs: number;
    }
  | { type: "tool_call"; item: Extract<ChatItem, { type: "tool_call" }>; idx: number }
  | { type: "thinking"; item: Extract<ChatItem, { type: "thinking" }>; idx: number };

@customElement("sc-message-thread")
export class ScMessageThread extends LitElement {
  @property({ type: Array }) items: ChatItem[] = [];
  @property({ type: Boolean }) isWaiting = false;
  @property({ type: String }) streamElapsed = "";
  @property({ type: Boolean }) historyLoading = false;

  @state() private showScrollPill = false;
  @query("#scroll-container") private scrollContainer!: HTMLElement;

  private _scrollHandler = (): void => {
    const el = this.scrollContainer;
    if (!el) return;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 60;
    if (this.showScrollPill === atBottom) this.showScrollPill = !atBottom;
  };

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
      gap: var(--sc-space-sm);
      scroll-behavior: smooth;
    }
    .bubble-wrapper {
      position: relative;
    }
    .bubble-wrapper:hover sc-message-actions {
      opacity: 1;
      transform: translateY(0);
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
    .avatar-icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-space-md);
      height: var(--sc-space-md);
      color: var(--sc-accent);
    }
    .avatar-icon svg {
      width: 100%;
      height: 100%;
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
    }
    .scroll-bottom-pill:hover {
      background: var(--sc-bg-elevated);
    }
    .pill-icon svg {
      width: 14px;
      height: 14px;
      vertical-align: -2px;
    }
    .waiting-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      align-self: flex-start;
    }
    .abort-btn {
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: transparent;
      color: var(--sc-text-muted);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
    }
    .abort-btn:hover {
      color: var(--sc-error);
      border-color: var(--sc-error);
    }
    .history-skeleton {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      padding: var(--sc-space-lg) 0;
    }
    .branch-nav {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      margin-top: var(--sc-space-2xs);
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }
    .branch-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 20px;
      height: 20px;
      padding: 0;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      color: var(--sc-text-muted);
      cursor: pointer;
    }
    .branch-btn:hover {
      color: var(--sc-accent);
      border-color: var(--sc-accent);
    }
    .branch-btn svg {
      width: 12px;
      height: 12px;
    }
    .branch-label {
      font-variant-numeric: tabular-nums;
      min-width: 32px;
      text-align: center;
    }
    .reaction-pills {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-2xs);
      margin-top: var(--sc-space-2xs);
    }
    .reaction-pill {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      cursor: pointer;
    }
    .reaction-pill:hover {
      border-color: var(--sc-accent);
    }
    .reaction-pill.mine {
      border-color: var(--sc-accent);
      background: var(--sc-accent-subtle);
    }
    .reaction-emoji {
      font-size: var(--sc-text-sm);
    }
    .reaction-count {
      color: var(--sc-text-muted);
    }
    @media (max-width: 640px) {
      .messages {
        padding: var(--sc-space-sm);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .messages {
        scroll-behavior: auto;
      }
    }
  `;

  override firstUpdated(): void {
    this.scrollContainer?.addEventListener("scroll", this._scrollHandler, { passive: true });
  }
  override disconnectedCallback(): void {
    this.scrollContainer?.removeEventListener("scroll", this._scrollHandler);
    super.disconnectedCallback();
  }
  override updated(changed: Map<string, unknown>): void {
    if (changed.has("items") || changed.has("isWaiting")) {
      const el = this.scrollContainer;
      if (!el) return;
      if (el.scrollHeight - el.scrollTop - el.clientHeight < 80) this.scrollToBottom();
    }
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
      )
        return i;
    }
    return -1;
  }

  private _buildBlocks(): Block[] {
    const blocks: Block[] = [];
    let currentGroup: {
      role: "user" | "assistant";
      messages: Array<{ item: Extract<ChatItem, { type: "message" }>; idx: number }>;
      lastTs: number;
    } | null = null;
    let lastGroupTs: number | null = null;
    const flushGroup = (): void => {
      if (currentGroup && currentGroup.messages.length > 0) {
        blocks.push({
          type: "message-group",
          role: currentGroup.role,
          messages: currentGroup.messages,
          lastTs: currentGroup.lastTs,
        });
        lastGroupTs = currentGroup.lastTs;
        currentGroup = null;
      }
    };
    const maybeTimeDivider = (ts: number): void => {
      if (lastGroupTs != null && Math.abs(ts - lastGroupTs) > FIVE_MIN_MS)
        blocks.push({ type: "time-divider", ts });
    };
    for (let i = 0; i < this.items.length; i++) {
      const item = this.items[i];
      if (item.type === "message") {
        const ts = item.ts ?? 0;
        if (currentGroup && currentGroup.role === item.role) {
          currentGroup.messages.push({ item, idx: i });
          currentGroup.lastTs = ts;
        } else {
          flushGroup();
          maybeTimeDivider(ts);
          currentGroup = { role: item.role, messages: [{ item, idx: i }], lastTs: ts };
        }
      } else if (item.type === "tool_call") {
        flushGroup();
        blocks.push({ type: "tool_call", item, idx: i });
      } else if (item.type === "thinking") {
        flushGroup();
        blocks.push({ type: "thinking", item, idx: i });
      }
    }
    flushGroup();
    return blocks;
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
  private _onAbort(): void {
    this.dispatchEvent(new CustomEvent("sc-abort", { bubbles: true, composed: true }));
  }
  private _navigateBranch(idx: number, direction: number): void {
    this.dispatchEvent(
      new CustomEvent("sc-branch-navigate", {
        bubbles: true,
        composed: true,
        detail: { index: idx, direction },
      }),
    );
  }
  private _toggleReaction(idx: number, emoji: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-toggle-reaction", {
        bubbles: true,
        composed: true,
        detail: { index: idx, emoji },
      }),
    );
  }

  private _renderMessageGroup(
    block: Extract<Block, { type: "message-group" }>,
  ): ReturnType<typeof html> {
    const { role, messages, lastTs } = block;
    const lastAssistantIdx = this._findLastAssistantIdx();
    return html`
      <sc-message-group role=${role}>
        ${messages.map(({ item, idx }, i) => {
          const isStreaming =
            this.isWaiting && item.role === "assistant" && idx === lastAssistantIdx;
          return html`
            <div class="bubble-wrapper" id="msg-${idx}">
              <sc-message-actions
                .role=${item.role}
                .content=${item.content}
                .index=${idx}
              ></sc-message-actions>
              <sc-chat-bubble
                .content=${item.content}
                .role=${item.role}
                .streaming=${isStreaming}
                .showTail=${i === messages.length - 1}
                .isFirst=${i === 0}
                .isLast=${i === messages.length - 1}
                @contextmenu=${(ev: MouseEvent) => this._onContextMenu(ev, item)}
              >
                ${item.role === "user" && item.status
                  ? html`<sc-delivery-status
                      slot="status"
                      .status=${item.status}
                    ></sc-delivery-status>`
                  : nothing}
                ${item.ts != null ? html`<span slot="meta">${formatTime(item.ts)}</span>` : nothing}
              </sc-chat-bubble>
              ${item.reactions?.length
                ? html`
                    <div class="reaction-pills">
                      ${item.reactions.map(
                        (r: { emoji: string; count: number; mine: boolean }) => html`
                          <button
                            class="reaction-pill ${r.mine ? "mine" : ""}"
                            @click=${() => this._toggleReaction(idx, r.emoji)}
                            aria-label="${r.emoji} ${r.count}"
                          >
                            <span class="reaction-emoji">${r.emoji}</span
                            ><span class="reaction-count">${r.count}</span>
                          </button>
                        `,
                      )}
                    </div>
                  `
                : nothing}
              ${item.branchCount != null && item.branchCount > 1
                ? html`
                    <div class="branch-nav">
                      <button
                        class="branch-btn"
                        @click=${() => this._navigateBranch(idx, -1)}
                        aria-label="Previous branch"
                      >
                        ${icons["caret-left"] ?? icons.chevron}
                      </button>
                      <span class="branch-label"
                        >${(item.branchIndex ?? 0) + 1} / ${item.branchCount}</span
                      >
                      <button
                        class="branch-btn"
                        @click=${() => this._navigateBranch(idx, 1)}
                        aria-label="Next branch"
                      >
                        ${icons["caret-right"] ?? icons["chevron-right"]}
                      </button>
                    </div>
                  `
                : nothing}
            </div>
          `;
        })}
        ${role === "assistant"
          ? html`<div slot="avatar" class="avatar-icon" aria-hidden="true">
              ${icons["chat-circle"]}
            </div>`
          : nothing}
        <span slot="timestamp">${formatTime(lastTs)}</span>
      </sc-message-group>
    `;
  }

  override render() {
    const blocks = this._buildBlocks();
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
              ${blocks.map((block) => {
                if (block.type === "time-divider")
                  return html`<div class="time-divider"><span>${formatTime(block.ts)}</span></div>`;
                if (block.type === "message-group") return this._renderMessageGroup(block);
                if (block.type === "tool_call")
                  return html`<sc-tool-result
                    .tool=${block.item.name}
                    .status=${block.item.status === "completed"
                      ? block.item.result?.startsWith("Error")
                        ? "error"
                        : "success"
                      : "running"}
                    .content=${block.item.result ?? block.item.input ?? ""}
                  ></sc-tool-result>`;
                if (block.type === "thinking")
                  return html`<sc-reasoning-block
                    .content=${block.item.content}
                    .streaming=${block.item.streaming}
                    .duration=${block.item.duration ?? ""}
                  ></sc-reasoning-block>`;
                return nothing;
              })}
              ${this.isWaiting
                ? html`
                    <div class="waiting-row">
                      <sc-typing-indicator .elapsed=${this.streamElapsed}></sc-typing-indicator>
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
    "sc-message-thread": ScMessageThread;
  }
}
