import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./hu-code-block.js";
import "./hu-toast.js";
import { ScToast } from "./hu-toast.js";

@customElement("hu-chat-bubble")
export class ScChatBubble extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: String }) role: "user" | "assistant" = "assistant";
  @property({ type: Boolean }) streaming = false;
  @property({ type: Boolean }) completing = false;
  @property({ type: Boolean }) showTail = false;
  @property({ type: Boolean }) isLast = false;
  @property({ type: Boolean }) isFirst = false;
  @property({ type: Object })
  replyTo: { id: string; content: string; role: string } | null = null;

  @property({ type: Number }) ariaMessageOrdinal = 0;
  @property({ type: Number }) ariaMessageTotal = 0;

  @state() private _visibleContent = "";
  private _wordQueue: string[] = [];
  private _releaseTimer = 0;
  private _lastContentLength = 0;

  static override styles = css`
    @keyframes hu-bubble-send {
      0% {
        opacity: 0;
        transform: translateY(var(--hu-space-xl)) scale(0.88);
      }
      50% {
        opacity: 1;
        transform: translateY(calc(-1 * var(--hu-space-xs))) scale(1.03);
      }
      70% {
        transform: translateY(var(--hu-space-2xs)) scale(0.99);
      }
      100% {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes hu-bubble-receive {
      0% {
        opacity: 0;
        transform: translateY(var(--hu-space-md)) scale(0.96);
      }
      60% {
        opacity: 1;
        transform: translateY(calc(-1 * var(--hu-space-2xs))) scale(1.01);
      }
      100% {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes hu-cursor-glow {
      0%,
      100% {
        opacity: 1;
        box-shadow: 0 0 4px 1px color-mix(in srgb, var(--hu-accent) 40%, transparent);
      }
      50% {
        opacity: 0.4;
        box-shadow: 0 0 2px 0 color-mix(in srgb, var(--hu-accent) 15%, transparent);
      }
    }

    @keyframes hu-cursor-fade-out {
      from {
        opacity: 1;
        box-shadow: 0 0 8px 2px color-mix(in srgb, var(--hu-accent) 50%, transparent);
      }
      to {
        opacity: 0;
        box-shadow: 0 0 0 0 transparent;
      }
    }

    @keyframes hu-bubble-settle {
      0% {
        transform: scale(1);
      }
      40% {
        transform: scale(1.008);
      }
      100% {
        transform: scale(1);
      }
    }

    :host {
      display: block;
    }

    .bubble {
      position: relative;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      line-height: var(--hu-leading-relaxed);
      padding: var(--hu-space-sm) var(--hu-space-md);
      box-shadow: var(--hu-shadow-sm);
    }

    .bubble.role-user {
      margin-left: auto;
      max-width: 75%;
      background: linear-gradient(
        135deg,
        color-mix(in srgb, var(--hu-accent) 85%, transparent),
        var(--hu-accent-hover)
      );
      color: var(--hu-on-accent-text, var(--hu-on-accent));
      border-radius: var(--hu-radius-xl) var(--hu-radius-xl) var(--hu-radius-sm) var(--hu-radius-xl);
      animation: hu-bubble-send var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    .bubble.role-user::after {
      content: "";
      display: var(--tail-display, none);
      position: absolute;
      bottom: -1px;
      right: calc(-1 * var(--hu-space-sm));
      width: var(--hu-space-sm);
      height: calc(var(--hu-space-sm) + var(--hu-space-2xs));
      background: var(--hu-accent);
      clip-path: polygon(0 0, 100% 100%, 0 100%);
    }

    .bubble.role-user.show-tail::after {
      --tail-display: block;
    }

    .bubble.role-assistant {
      margin-right: auto;
      max-width: 85%;
      background: color-mix(in srgb, var(--hu-surface-container) 65%, transparent);
      backdrop-filter: blur(var(--hu-blur-md));
      -webkit-backdrop-filter: blur(var(--hu-blur-md));
      color: var(--hu-text);
      border: 1px solid color-mix(in srgb, var(--hu-color-white) 8%, transparent);
      border-radius: var(--hu-radius-xl) var(--hu-radius-xl) var(--hu-radius-xl) var(--hu-radius-sm);
      box-shadow: var(--hu-shadow-xs);
      animation: hu-bubble-receive var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    .bubble.role-assistant::after {
      content: "";
      display: var(--tail-display, none);
      position: absolute;
      bottom: -1px;
      left: calc(-1 * var(--hu-space-sm));
      width: var(--hu-space-sm);
      height: calc(var(--hu-space-sm) + var(--hu-space-2xs));
      background: color-mix(in srgb, var(--hu-surface-container) 65%, transparent);
      clip-path: polygon(100% 0, 100% 100%, 0 100%);
    }

    .bubble.role-assistant.show-tail::after {
      --tail-display: block;
    }

    @media (prefers-reduced-transparency: reduce) {
      .bubble.role-assistant {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-surface-container);
        border: 1px solid var(--hu-border-subtle);
      }
      .bubble.role-assistant::after {
        background: var(--hu-surface-container);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .bubble.role-user,
      .bubble.role-assistant {
        animation: none;
      }
    }

    .content {
      position: relative;
      z-index: 1;
      overflow: hidden;
    }

    /* Assistant markdown content */
    .bubble.role-assistant .md-content {
      color: var(--hu-text);
    }

    .bubble.role-assistant .md-content a {
      color: var(--hu-accent);
      text-decoration: none;
    }

    .bubble.role-assistant .md-content a:hover {
      text-decoration: underline;
    }

    .bubble.role-assistant .md-content code.inline {
      font-family: var(--hu-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--hu-radius-sm);
      background: var(--hu-bg-inset);
    }

    /* User bubble content */
    .bubble.role-user .md-content {
      color: var(--hu-on-accent);
    }

    .bubble.role-user .md-content a {
      color: var(--hu-on-accent);
      text-decoration: underline;
    }

    .bubble.role-user .md-content a:hover {
      text-decoration: underline;
    }

    .bubble.role-user .md-content code.inline {
      font-family: var(--hu-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--hu-radius-sm);
      background: color-mix(in srgb, var(--hu-on-accent) 90%, transparent);
    }

    /* Shared markdown styles */
    .md-heading {
      font-weight: var(--hu-weight-semibold);
      margin: var(--hu-space-sm) 0 var(--hu-space-xs);
    }

    .md-heading:first-child {
      margin-top: 0;
    }

    .md-content h1.md-heading {
      font-size: var(--hu-text-2xl);
    }
    .md-content h2.md-heading {
      font-size: var(--hu-text-xl);
    }
    .md-content h3.md-heading {
      font-size: var(--hu-text-lg);
    }
    .md-content h4.md-heading,
    .md-content h5.md-heading,
    .md-content h6.md-heading {
      font-size: var(--hu-text-base);
    }

    .md-paragraph {
      margin: var(--hu-space-xs) 0;
    }

    .md-blockquote {
      border-left: 3px solid var(--hu-accent);
      padding-left: var(--hu-space-md);
      margin: var(--hu-space-sm) 0;
      color: var(--hu-text-muted);
      font-style: italic;
    }

    .bubble.role-user .md-blockquote {
      border-left-color: var(--hu-on-accent);
      color: color-mix(in srgb, var(--hu-on-accent) 85%, transparent);
    }

    .md-list {
      margin: var(--hu-space-sm) 0;
      padding-left: var(--hu-space-lg);
    }

    .md-list-item {
      margin-bottom: var(--hu-space-2xs);
    }

    .md-table-scroll {
      overflow-x: auto;
      -webkit-overflow-scrolling: touch;
      margin: var(--hu-space-sm) 0;
      border-radius: var(--hu-radius-md);
      border: 1px solid var(--hu-border-subtle);
    }

    .md-table-scroll .md-table {
      margin: 0;
      border: none;
    }

    .md-table {
      border-collapse: collapse;
      width: 100%;
      margin: var(--hu-space-sm) 0;
    }

    .md-table tbody tr:nth-child(even) {
      background: color-mix(in srgb, var(--hu-surface-container) 40%, transparent);
    }

    .md-table tbody tr:hover {
      background: var(--hu-hover-overlay);
    }

    .md-table th {
      position: sticky;
      top: 0;
      z-index: 1;
      background: var(--hu-surface-container-high);
    }

    .md-table th,
    .md-table td {
      border: 1px solid var(--hu-border);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      text-align: left;
    }

    .bubble.role-user .md-table th,
    .bubble.role-user .md-table td {
      border-color: color-mix(in srgb, var(--hu-on-accent) 30%, transparent);
    }

    .bubble.role-user .md-table tbody tr:nth-child(even) {
      background: color-mix(in srgb, var(--hu-on-accent) 8%, transparent);
    }

    .bubble.role-user .md-table tbody tr:hover {
      background: color-mix(in srgb, var(--hu-on-accent) 15%, transparent);
    }

    .bubble.role-user .md-table th {
      background: color-mix(in srgb, var(--hu-on-accent) 25%, transparent);
    }

    .md-table th {
      font-weight: var(--hu-weight-semibold);
    }

    .md-hr {
      border: none;
      border-top: 1px solid var(--hu-border);
      margin: var(--hu-space-md) 0;
    }

    .bubble.role-user .md-hr {
      border-top-color: color-mix(in srgb, var(--hu-on-accent) 40%, transparent);
    }

    .md-content img {
      max-width: 100%;
      border-radius: var(--hu-radius-md);
    }

    .md-image-clickable {
      cursor: pointer;
      display: inline-block;
    }

    .md-image-clickable:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .cursor {
      display: inline-block;
      width: 2px;
      height: 1.1em;
      background: var(--hu-accent);
      margin-left: var(--hu-space-2xs);
      vertical-align: text-bottom;
      border-radius: var(--hu-radius-xs);
      animation: hu-cursor-glow var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .cursor.completing {
      animation: hu-cursor-fade-out var(--hu-duration-normal) var(--hu-ease-out) forwards;
    }

    .bubble.role-user .cursor {
      background: var(--hu-on-accent);
    }

    .bubble.settling {
      animation: hu-bubble-settle var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
    }

    @media (prefers-reduced-motion: reduce) {
      .cursor,
      .cursor.completing {
        animation: none;
      }
      .bubble.settling {
        animation: none;
      }
    }

    .footer {
      display: flex;
      align-items: center;
      justify-content: flex-end;
      gap: var(--hu-space-sm);
      margin-top: var(--hu-space-xs);
      min-height: 1.25em;
    }

    .bubble.role-user .footer {
      justify-content: flex-end;
    }

    .bubble.role-assistant .footer {
      justify-content: flex-start;
    }

    .meta-slot {
      opacity: 0;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .bubble.role-user .meta-slot {
      color: color-mix(in srgb, var(--hu-on-accent) 80%, transparent);
    }

    .bubble:hover .meta-slot {
      opacity: 1;
    }

    @media (prefers-reduced-motion: reduce) {
      .meta-slot {
        transition: none;
      }
    }

    .bubble:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: var(--hu-focus-ring-offset);
    }

    /* Threaded reply quote */
    .reply-quote {
      display: block;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      margin-bottom: var(--hu-space-xs);
      border-left: 3px solid var(--hu-accent);
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
      border-radius: 0 var(--hu-radius-sm) var(--hu-radius-sm) 0;
      cursor: pointer;
      font-family: var(--hu-font);
      max-width: 100%;
    }
    .reply-quote:hover {
      background: color-mix(in srgb, var(--hu-accent) 12%, transparent);
    }
    .reply-role {
      display: block;
      font-size: var(--hu-text-2xs, 0.625rem);
      color: var(--hu-text-muted);
      text-transform: capitalize;
      margin-bottom: var(--hu-space-2xs);
    }
    .reply-preview {
      display: block;
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .bubble.role-user .reply-quote {
      border-left-color: var(--hu-on-accent);
      background: color-mix(in srgb, var(--hu-on-accent) 15%, transparent);
    }
    .bubble.role-user .reply-quote:hover {
      background: color-mix(in srgb, var(--hu-on-accent) 20%, transparent);
    }
    .bubble.role-user .reply-role {
      color: color-mix(in srgb, var(--hu-on-accent) 80%, transparent);
    }
    .bubble.role-user .reply-preview {
      color: var(--hu-on-accent);
    }
  `;

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this._clearReleaseTimer();
  }

  override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has("content") || changed.has("streaming")) {
      this._updateWordBuffer();
    }
  }

  private _updateWordBuffer(): void {
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!this.streaming || this.role === "user" || reducedMotion) {
      this._visibleContent = this.content;
      this._wordQueue = [];
      this._clearReleaseTimer();
      this._lastContentLength = this.content.length;
      return;
    }

    const newChars = this.content.slice(this._lastContentLength);
    this._lastContentLength = this.content.length;
    if (!newChars) return;

    const newWords = newChars.match(/\S+\s*/g) ?? [newChars];
    this._wordQueue.push(...newWords);

    if (!this._releaseTimer) {
      this._releaseNextWord();
    }
  }

  /** Batch size for word release — re-render markdown every ~100ms instead of per-word. */
  private static readonly _STREAM_BATCH_SIZE = 5;
  private static readonly _STREAM_BATCH_MS = 100;

  private _releaseNextWord(): void {
    if (this._wordQueue.length === 0) {
      this._releaseTimer = 0;
      return;
    }
    const batchSize = Math.min(ScChatBubble._STREAM_BATCH_SIZE, this._wordQueue.length);
    for (let i = 0; i < batchSize; i++) {
      const word = this._wordQueue.shift()!;
      this._visibleContent += word;
    }
    this.requestUpdate();

    this._releaseTimer = window.setTimeout(
      () => this._releaseNextWord(),
      ScChatBubble._STREAM_BATCH_MS,
    );
  }

  private _clearReleaseTimer(): void {
    if (this._releaseTimer) {
      window.clearTimeout(this._releaseTimer);
      this._releaseTimer = 0;
    }
  }

  private _copyCode(code: string): void {
    navigator.clipboard?.writeText(code).then(
      () => {
        ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      },
      () => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      },
    );
  }

  override render() {
    const classes = [
      "bubble",
      `role-${this.role}`,
      this.showTail ? "show-tail" : "",
      this.completing ? "settling" : "",
      this.streaming ? "streaming-active" : "",
    ]
      .filter(Boolean)
      .join(" ");
    const isUser = this.role === "user";
    const showCursor = this.streaming || this.completing;

    return html`
      <div
        class=${classes}
        role="article"
        aria-label=${this.ariaMessageOrdinal && this.ariaMessageTotal
          ? `Message ${this.ariaMessageOrdinal} of ${this.ariaMessageTotal}, from ${isUser ? "user" : "assistant"}`
          : isUser
            ? "Your message"
            : "Assistant message"}
        aria-busy=${this.streaming}
        tabindex="0"
      >
        <div class="content">
          ${this.replyTo
            ? html`
                <div
                  class="reply-quote"
                  role="button"
                  tabindex="0"
                  @click=${() =>
                    this.dispatchEvent(
                      new CustomEvent("scroll-to-message", {
                        bubbles: true,
                        composed: true,
                        detail: { id: this.replyTo!.id },
                      }),
                    )}
                  @keydown=${(e: KeyboardEvent) => {
                    if (e.key === "Enter" || e.key === " ") {
                      e.preventDefault();
                      this.dispatchEvent(
                        new CustomEvent("scroll-to-message", {
                          bubbles: true,
                          composed: true,
                          detail: { id: this.replyTo!.id },
                        }),
                      );
                    }
                  }}
                >
                  <span class="reply-role">${this.replyTo.role}</span>
                  <span class="reply-preview"
                    >${this.replyTo.content.length > 80
                      ? this.replyTo.content.slice(0, 80) + "\u2026"
                      : this.replyTo.content}</span
                  >
                </div>
              `
            : nothing}
          ${renderMarkdown(
            this.streaming && this.role === "assistant" ? this._visibleContent : this.content,
            {
              onCopyCode: (code) => this._copyCode(code),
              onImageClick: (src) =>
                this.dispatchEvent(
                  new CustomEvent("open-image", { detail: { src }, bubbles: true, composed: true }),
                ),
              streaming: this.streaming,
            },
          )}
          ${showCursor
            ? html`<span
                class="cursor ${this.completing ? "completing" : ""}"
                aria-hidden="true"
              ></span>`
            : nothing}
        </div>
        <div class="footer">
          <slot name="status"></slot>
          <span class="meta-slot"><slot name="meta"></slot></span>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-chat-bubble": ScChatBubble;
  }
}
