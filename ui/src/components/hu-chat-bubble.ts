import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { renderMarkdown } from "../lib/markdown.js";
import { icons } from "../icons.js";
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
  @state() private _outlineOpen = false;
  @state() private _headings: Array<{ level: number; text: string; id: string }> = [];
  private _wordQueue: string[] = [];
  private _releaseTimer = 0;
  private _lastContentLength = 0;
  /** Sliding window of word counts keyed by arrival time (for adaptive streaming pace). */
  private _arrivalSamples: Array<{ at: number; count: number }> = [];

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

    @keyframes hu-send-confirm {
      0% {
        box-shadow: 0 0 0 4px color-mix(in srgb, var(--hu-accent) 25%, transparent);
      }
      100% {
        box-shadow: 0 0 0 0 transparent;
      }
    }

    :host {
      display: block;
      min-width: 0;
      contain: layout style;
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
        box-shadow: 0 0 0 0 transparent;
      }
      40% {
        transform: scale(1.015);
        box-shadow: 0 0 8px 2px color-mix(in srgb, var(--hu-accent) 15%, transparent);
      }
      100% {
        transform: scale(1);
        box-shadow: 0 0 0 0 transparent;
      }
    }

    .bubble {
      position: relative;
      width: fit-content;
      min-width: 3rem;
      font-family: var(--hu-font);
      font-size: var(--hu-text-base);
      line-height: var(--hu-leading-relaxed);
      padding: var(--hu-space-sm) var(--hu-space-md);
    }

    .bubble.role-user {
      margin-inline-start: auto;
      max-width: 80%;
      background: var(--hu-surface-container-high);
      color: var(--hu-text);
      border-radius: var(--hu-radius-xl) var(--hu-radius-xl) var(--hu-radius-sm) var(--hu-radius-xl);
      animation:
        hu-bubble-send var(--hu-duration-normal)
          var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both,
        hu-send-confirm var(--hu-duration-slow) var(--hu-ease-out) both;
    }

    .bubble.role-assistant {
      margin-inline-end: auto;
      max-width: 100%;
      background: transparent;
      color: var(--hu-text);
      border: none;
      border-radius: 0;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      animation: hu-bubble-receive var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
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

    .bubble.role-assistant .md-content {
      color: var(--hu-text);

      & a {
        color: var(--hu-accent);
        text-decoration: none;
        &:hover {
          text-decoration: underline;
        }
      }

      & code.inline {
        font-family: var(--hu-font-mono);
        font-size: 0.9em;
        padding: 0.1em 0.35em;
        border-radius: var(--hu-radius-sm);
        background: var(--hu-bg-inset);
      }
    }

    .bubble.role-user .md-content {
      color: var(--hu-text);

      & a {
        color: var(--hu-accent);
        text-decoration: none;
        &:hover {
          text-decoration: underline;
        }
      }

      & code.inline {
        font-family: var(--hu-font-mono);
        font-size: 0.9em;
        padding: 0.1em 0.35em;
        border-radius: var(--hu-radius-sm);
        background: var(--hu-bg-inset);
      }
    }

    /* Shared markdown styles */
    .md-heading {
      font-weight: var(--hu-weight-semibold);
      margin: var(--hu-space-sm) 0 var(--hu-space-xs);

      &:first-child {
        margin-top: 0;
      }
    }

    .md-content {
      & h1.md-heading {
        font-size: var(--hu-text-lg);
      }
      & h2.md-heading {
        font-size: var(--hu-text-base);
        font-weight: var(--hu-weight-semibold);
      }
      & h3.md-heading {
        font-size: var(--hu-text-base);
      }
      & h4.md-heading,
      & h5.md-heading,
      & h6.md-heading {
        font-size: var(--hu-text-sm);
      }
    }

    .md-paragraph {
      margin: var(--hu-space-xs) 0;
    }

    .md-blockquote {
      border-left: 3px solid var(--hu-accent);
      padding-inline-start: var(--hu-space-md);
      margin: var(--hu-space-sm) 0;
      color: var(--hu-text);
      font-style: italic;
    }

    .bubble.role-user .md-blockquote {
      border-left-color: var(--hu-accent);
      color: var(--hu-text);
    }

    .md-list {
      margin: var(--hu-space-sm) 0;
      padding-inline-start: var(--hu-space-lg);
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
      border-color: var(--hu-border);
    }

    .bubble.role-user .md-table tbody tr:nth-child(even) {
      background: color-mix(in srgb, var(--hu-surface-container) 40%, transparent);
    }

    .bubble.role-user .md-table tbody tr:hover {
      background: var(--hu-hover-overlay);
    }

    .bubble.role-user .md-table th {
      background: var(--hu-surface-container-high);
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
      border-top-color: var(--hu-border);
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
      margin-inline-start: var(--hu-space-2xs);
      vertical-align: text-bottom;
      border-radius: var(--hu-radius-xs);
      animation: hu-cursor-glow var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .cursor.completing {
      animation: hu-cursor-fade-out var(--hu-duration-normal) var(--hu-ease-out) forwards;
    }

    .bubble.role-user .cursor {
      background: var(--hu-accent);
    }

    .bubble.settling {
      animation: hu-bubble-settle var(--hu-duration-slow)
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
      opacity: 0.5;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .bubble.role-user .meta-slot {
      color: var(--hu-text-muted);
    }

    .bubble:hover .meta-slot,
    .bubble:focus-within .meta-slot {
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

    .outline-toggle {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      margin-bottom: var(--hu-space-xs);
      background: color-mix(in srgb, var(--hu-accent) 6%, transparent);
      border: 1px solid color-mix(in srgb, var(--hu-accent) 15%, transparent);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-2xs, 0.625rem);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }
    .outline-toggle:hover {
      color: var(--hu-accent);
      border-color: var(--hu-accent);
    }
    .outline-toggle:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .outline-toggle:active {
      transform: scale(0.97);
    }
    .outline-toggle svg {
      width: var(--hu-icon-xs);
      height: var(--hu-icon-xs);
    }
    .outline-list {
      list-style: none;
      padding: 0 0 var(--hu-space-xs) 0;
      margin: 0;
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }
    .outline-item {
      display: block;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      text-decoration: none;
      border-radius: var(--hu-radius-sm);
      cursor: pointer;
      background: transparent;
      border: none;
      text-align: left;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }
    .outline-item:hover {
      color: var(--hu-accent);
      background: color-mix(in srgb, var(--hu-accent) 6%, transparent);
    }
    .outline-item:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: -2px;
    }
    .outline-item[data-level="2"] {
      padding-inline-start: var(--hu-space-md);
    }
    .outline-item[data-level="3"] {
      padding-inline-start: var(--hu-space-lg);
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
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
    }
    .reply-quote:hover {
      background: color-mix(in srgb, var(--hu-accent) 12%, transparent);
    }
    .reply-quote:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .reply-role {
      display: block;
      font-size: var(--hu-text-2xs, 0.625rem);
      color: var(--hu-text);
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
      border-left-color: var(--hu-accent);
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
    }
    .bubble.role-user .reply-quote:hover {
      background: color-mix(in srgb, var(--hu-accent) 12%, transparent);
    }
    .bubble.role-user .reply-role {
      color: var(--hu-text-secondary);
    }
    .bubble.role-user .reply-preview {
      color: var(--hu-text);
    }
  `;

  override disconnectedCallback(): void {
    this._clearReleaseTimer();
    super.disconnectedCallback();
  }

  private static readonly _OUTLINE_CHAR_THRESHOLD = 2000;

  override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has("content") || changed.has("streaming")) {
      this._updateWordBuffer();
    }
    if (changed.has("content") && !this.streaming && this.role === "assistant") {
      this._extractHeadings();
    }
  }

  private _extractHeadings(): void {
    if (this.content.length < ScChatBubble._OUTLINE_CHAR_THRESHOLD) {
      this._headings = [];
      return;
    }
    const headings: Array<{ level: number; text: string; id: string }> = [];
    const re = /^(#{1,3})\s+(.+)$/gm;
    let m: RegExpExecArray | null;
    while ((m = re.exec(this.content)) !== null) {
      const level = m[1].length;
      const text = m[2].trim();
      const id = `h-${headings.length}`;
      headings.push({ level, text, id });
    }
    this._headings = headings;
  }

  private _scrollToHeading(text: string): void {
    const root = this.shadowRoot;
    if (!root) return;
    const headingEls = root.querySelectorAll(".md-heading");
    for (const el of headingEls) {
      if (el.textContent?.trim() === text) {
        el.scrollIntoView({ behavior: "smooth", block: "nearest" });
        return;
      }
    }
  }

  private _updateWordBuffer(): void {
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!this.streaming || this.role === "user" || reducedMotion) {
      this._visibleContent = this.content;
      this._wordQueue = [];
      this._arrivalSamples = [];
      this._clearReleaseTimer();
      this._lastContentLength = this.content.length;
      return;
    }

    const newChars = this.content.slice(this._lastContentLength);
    this._lastContentLength = this.content.length;
    if (!newChars) return;

    const newWords = newChars.match(/\S+\s*/g) ?? [newChars];
    this._wordQueue.push(...newWords);
    const now = performance.now();
    this._arrivalSamples.push({ at: now, count: newWords.length });
    this._pruneArrivalSamples(now);

    if (!this._releaseTimer) {
      this._releaseNextWord();
    }
  }

  /** Slow path: batch size / interval when arrivals are sparse (typing feel). */
  private static readonly _STREAM_BATCH_SIZE = 5;
  private static readonly _STREAM_BATCH_MS = 100;
  /** Words summed over this window set the adaptive mode. */
  private static readonly _ARRIVAL_WINDOW_MS = 200;
  /** Above this many words in the window → drain the whole pending queue (fast model). */
  private static readonly _HIGH_THROUGHPUT_WORDS = 10;
  /** Below this many words in the window → word-by-word batching. */
  private static readonly _LOW_THROUGHPUT_WORDS = 5;
  /** Between low and high: still drain queue, but pace slightly vs instant re-entry. */
  private static readonly _MEDIUM_DRAIN_DELAY_MS = 16;

  private _pruneArrivalSamples(now: number): void {
    const cutoff = now - ScChatBubble._ARRIVAL_WINDOW_MS;
    while (this._arrivalSamples.length > 0 && this._arrivalSamples[0].at < cutoff) {
      this._arrivalSamples.shift();
    }
  }

  private _recentArrivalWordCount(now: number): number {
    this._pruneArrivalSamples(now);
    let sum = 0;
    for (const s of this._arrivalSamples) {
      sum += s.count;
    }
    return sum;
  }

  private _releaseNextWord(): void {
    if (this._wordQueue.length === 0) {
      this._releaseTimer = 0;
      return;
    }
    const now = performance.now();
    const recent = this._recentArrivalWordCount(now);

    let batchSize: number;
    let delayMs: number;
    if (recent > ScChatBubble._HIGH_THROUGHPUT_WORDS) {
      batchSize = this._wordQueue.length;
      delayMs = 0;
    } else if (recent < ScChatBubble._LOW_THROUGHPUT_WORDS) {
      batchSize = Math.min(ScChatBubble._STREAM_BATCH_SIZE, this._wordQueue.length);
      delayMs = ScChatBubble._STREAM_BATCH_MS;
    } else {
      batchSize = this._wordQueue.length;
      delayMs = ScChatBubble._MEDIUM_DRAIN_DELAY_MS;
    }

    for (let i = 0; i < batchSize; i++) {
      const word = this._wordQueue.shift()!;
      this._visibleContent += word;
    }
    this.requestUpdate();

    this._releaseTimer = window.setTimeout(() => this._releaseNextWord(), delayMs);
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
        aria-label=${
          this.ariaMessageOrdinal && this.ariaMessageTotal
            ? `Message ${this.ariaMessageOrdinal} of ${this.ariaMessageTotal}, from ${isUser ? "user" : "assistant"}`
            : isUser
              ? "Your message"
              : "Assistant message"
        }
        aria-busy=${this.streaming}
        tabindex="0"
      >
        <div class="content">
          ${
            this._headings.length > 0
              ? html`
                  <button
                    class="outline-toggle"
                    type="button"
                    @click=${() => (this._outlineOpen = !this._outlineOpen)}
                    aria-label="Toggle section outline"
                  >
                    ${icons["list-bullets"] ?? icons.list ?? nothing}
                    ${this._outlineOpen ? "Hide" : "Outline"} (${this._headings.length})
                  </button>
                  ${
                    this._outlineOpen
                      ? html`
                          <ul class="outline-list" role="navigation" aria-label="Section outline">
                            ${this._headings.map(
                              (h) => html`
                                <li>
                                  <button
                                    class="outline-item"
                                    data-level=${h.level}
                                    @click=${() => this._scrollToHeading(h.text)}
                                  >
                                    ${h.text}
                                  </button>
                                </li>
                              `,
                            )}
                          </ul>
                        `
                      : nothing
                  }
                `
              : nothing
          }
          ${
            this.replyTo
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
                      >${
                        this.replyTo.content.length > 80
                          ? this.replyTo.content.slice(0, 80) + "\u2026"
                          : this.replyTo.content
                      }</span
                    >
                  </div>
                `
              : nothing
          }
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
          ${
            showCursor
              ? html`<span
                  class="cursor ${this.completing ? "completing" : ""}"
                  aria-hidden="true"
                ></span>`
              : nothing
          }
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
