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
import type { ChatItem, ArtifactData } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import { formatTime, formatTimestampForDivider } from "../utils.js";

const FIVE_MIN_MS = 5 * 60 * 1000;
const SWIPE_START_THRESHOLD = 10;
const SWIPE_ACTION_THRESHOLD = 60;
const SWIPE_RESISTANCE = 0.6;

function getTimeGreeting(): string {
  const hour = new Date().getHours();
  if (hour < 5) return "Late night session?";
  if (hour < 12) return "Good morning.";
  if (hour < 17) return "Good afternoon.";
  if (hour < 21) return "Good evening.";
  return "Burning the midnight oil?";
}

const HERO_SUGGESTIONS = [
  { label: "Brainstorm ideas", icon: "zap" as const },
  { label: "Write something", icon: "pencil" as const },
  { label: "Debug a problem", icon: "wrench" as const },
  { label: "Explain a concept", icon: "book-open" as const },
];

const DEMO_FOLLOWUP_SUGGESTIONS = [
  "Tell me more about this",
  "Can you show an example?",
  "What are the alternatives?",
];

const VALUE_TO_ICON: Record<string, keyof typeof icons> = {
  like: "thumbs-up",
  dislike: "thumbs-down",
  heart: "heart",
  copy: "copy",
  bookmark: "bookmark-simple",
};

const ARTIFACT_TYPE_ICON: Record<ArtifactData["type"], keyof typeof icons> = {
  code: "code",
  document: "file-text",
  html: "monitor",
  diagram: "chart-line",
};

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
  @property({ type: Boolean }) isCompleting = false;
  @property({ type: String }) streamElapsed = "";
  @property({ type: Boolean }) historyLoading = false;
  @property({ type: Boolean }) hasEarlierMessages = false;
  @property({ type: Boolean }) loadingEarlier = false;
  @property({ type: Array }) suggestions: string[] = [];
  @property({ type: Array }) artifacts: ArtifactData[] = [];

  @state() private showScrollPill = false;
  @state() private _imageViewerOpen = false;
  @state() private _imageViewerSrc = "";
  @state() private _focusedMessageIndex = -1;
  @query("#scroll-container") private scrollContainer!: HTMLElement;
  private _smoothScrollRaf = 0;
  private _swipeState: {
    idx: number;
    startX: number;
    startY: number;
    currentX: number;
  } | null = null;
  private _swipeRaf = 0;

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
      gap: var(--sc-space-lg);
      scroll-behavior: smooth;
      overscroll-behavior: contain;
      scroll-snap-type: y proximity;
      scroll-padding-top: var(--sc-space-xl);
    }
    .message-wrapper {
      position: relative;
      overflow: hidden;
    }
    .swipe-content {
      position: relative;
      z-index: 1;
      transition: transform var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
    }
    .message-wrapper.swiping .swipe-content {
      transition: none;
    }
    .swipe-action {
      position: absolute;
      top: 50%;
      transform: translateY(-50%);
      display: flex;
      align-items: center;
      justify-content: center;
      width: 4rem;
      height: 2.5rem;
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-accent);
      background: var(--sc-accent-subtle);
      border-radius: var(--sc-radius);
      z-index: 0;
    }
    .swipe-action.left {
      left: var(--sc-space-sm);
    }
    .swipe-action.right {
      right: var(--sc-space-sm);
    }
    .bubble-wrapper {
      position: relative;
    }
    .bubble-wrapper:hover sc-message-actions {
      opacity: 1;
      transform: translateY(0);
    }
    @media (hover: none) {
      .bubble-wrapper sc-message-actions {
        opacity: var(--sc-opacity-overlay-heavy);
        transform: translateY(0);
      }
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
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      white-space: nowrap;
    }
    @keyframes sc-pill-bounce {
      0% {
        transform: translateX(-50%) translateY(var(--sc-space-lg)) scale(0.8);
        opacity: 0;
      }
      60% {
        transform: translateX(-50%) translateY(calc(-1 * var(--sc-space-xs))) scale(1.05);
        opacity: 1;
      }
      100% {
        transform: translateX(-50%) translateY(0) scale(1);
        opacity: 1;
      }
    }
    .scroll-bottom-pill {
      position: absolute;
      bottom: 5.625rem; /* sc-lint-ok: scroll-to-bottom offset above composer */
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
      animation: sc-pill-bounce var(--sc-duration-slow)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .scroll-bottom-pill:hover {
      background: var(--sc-hover-overlay);
    }
    .pill-icon svg {
      width: 0.875rem;
      height: 0.875rem;
      vertical-align: -0.125rem;
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
      font-size: var(--sc-text-2xs, 0.625rem);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }
    .branch-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
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
      width: 0.75rem;
      height: 0.75rem;
    }
    .branch-label {
      font-variant-numeric: tabular-nums;
      min-width: 2rem;
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
    .reaction-icon {
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .reaction-icon svg {
      width: 100%;
      height: 100%;
    }
    .reaction-fallback {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .reaction-count {
      color: var(--sc-text-muted);
    }
    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      .messages {
        padding: var(--sc-space-sm);
      }
    }
    .pull-to-load {
      position: sticky;
      top: 0;
      z-index: 2;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: var(--sc-space-sm) 0;
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text-muted);
      transform-origin: center top;
      transition: transform var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
    }
    .pull-to-load::before {
      content: "Pull to load earlier";
    }
    .load-earlier {
      display: flex;
      justify-content: center;
      padding: var(--sc-space-md) 0;
    }
    .load-earlier-btn {
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: transparent;
      color: var(--sc-text-muted);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      cursor: pointer;
    }
    .load-earlier-btn:hover {
      color: var(--sc-accent);
      border-color: var(--sc-accent);
    }
    .load-earlier-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .artifact-cards {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-xs);
      margin-top: var(--sc-space-sm);
    }
    .artifact-card {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: color-mix(in srgb, var(--sc-surface-container) 80%, transparent);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }
    .artifact-card:hover {
      border-color: var(--sc-accent);
      background: color-mix(in srgb, var(--sc-accent) 8%, transparent);
    }
    .artifact-card:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .artifact-card .artifact-icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      color: var(--sc-text-muted);
    }
    .artifact-card .artifact-icon svg {
      width: 100%;
      height: 100%;
    }
    .artifact-card:hover .artifact-icon {
      color: var(--sc-accent);
    }

    /* Empty state hero */
    .hero {
      flex: 1;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: var(--sc-space-xl);
      padding: var(--sc-space-2xl) var(--sc-space-lg);
      text-align: center;
      animation: sc-hero-enter var(--sc-duration-slow) var(--sc-ease-out) both;
    }
    @keyframes sc-hero-enter {
      from {
        opacity: 0;
        transform: translateY(var(--sc-space-md));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    .hero-greeting {
      font-family: var(--sc-font);
      font-size: var(--sc-text-3xl, 1.875rem);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      letter-spacing: -0.02em;
      line-height: 1.2;
    }
    .hero-sub {
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      max-width: 24rem;
    }
    .hero-suggestions {
      display: flex;
      flex-wrap: wrap;
      justify-content: center;
      gap: var(--sc-space-sm);
    }
    .hero-chip {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-sm) var(--sc-space-lg);
      background: color-mix(in srgb, var(--sc-surface-container) 50%, transparent);
      backdrop-filter: blur(var(--sc-blur-sm, 8px));
      -webkit-backdrop-filter: blur(var(--sc-blur-sm, 8px));
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast),
        background var(--sc-duration-fast),
        transform var(--sc-duration-fast);
    }
    .hero-chip:hover {
      border-color: var(--sc-accent);
      background: color-mix(in srgb, var(--sc-accent) 8%, transparent);
      transform: translateY(-1px);
    }
    .hero-chip:active {
      transform: scale(0.97);
    }
    .hero-chip:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .hero-chip svg {
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      color: var(--sc-accent);
    }
    @media (prefers-reduced-transparency: reduce) {
      .hero-chip {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--sc-surface-container);
      }
    }
    /* 3d: Follow-up suggestion chips */
    .suggestions {
      display: flex;
      flex-direction: row;
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) 0;
      align-self: flex-start;
    }
    .suggestion-chip {
      display: inline-flex;
      align-items: center;
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: transparent;
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast),
        color var(--sc-duration-fast);
      animation: sc-suggestion-enter var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .suggestion-chip:hover {
      border-color: var(--sc-accent);
      color: var(--sc-accent);
    }
    .suggestion-chip:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .suggestion-chip[data-stagger="1"] {
      animation-delay: var(--sc-stagger-delay, 50ms);
    }
    .suggestion-chip[data-stagger="2"] {
      animation-delay: calc(2 * var(--sc-stagger-delay, 50ms));
    }
    @keyframes sc-suggestion-enter {
      from {
        opacity: 0;
        transform: scale(0.95) translateY(var(--sc-space-xs));
      }
      to {
        opacity: 1;
        transform: scale(1) translateY(0);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .hero {
        animation: none;
      }
      .hero-chip {
        transition: none;
      }
      .messages {
        scroll-behavior: auto;
      }
      .suggestion-chip {
        animation: none;
      }
      .swipe-content {
        transition: none;
      }
    }
    /* 5d: Reaction pop-in animation */
    @keyframes sc-reaction-pop {
      0% {
        transform: scale(0);
        opacity: 0;
      }
      50% {
        transform: scale(1.3);
        opacity: 1;
      }
      70% {
        transform: scale(0.9);
      }
      100% {
        transform: scale(1);
        opacity: 1;
      }
    }
    .reaction-pill {
      animation: sc-reaction-pop var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .reaction-pill[data-stagger="0"] {
      animation-delay: 0ms;
    }
    .reaction-pill[data-stagger="1"] {
      animation-delay: var(--sc-stagger-delay, 50ms);
    }
    .reaction-pill[data-stagger="2"] {
      animation-delay: calc(2 * var(--sc-stagger-delay, 50ms));
    }
    .reaction-pill[data-stagger="3"] {
      animation-delay: calc(3 * var(--sc-stagger-delay, 50ms));
    }
    .reaction-count {
      transition: transform var(--sc-duration-fast) var(--sc-ease-out);
    }
    @media (prefers-reduced-motion: reduce) {
      .reaction-pill {
        animation: none;
      }
    }

    /* Phase 6a: Keyboard focus indicator */
    .message-item.keyboard-focused {
      position: relative;
    }
    .message-item.keyboard-focused::before {
      content: "";
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      width: 3px;
      background: var(--sc-accent);
      border-radius: var(--sc-radius-xs);
      animation: sc-focus-bar-enter var(--sc-duration-fast) var(--sc-ease-out) both;
    }
    @keyframes sc-focus-bar-enter {
      from {
        opacity: 0;
        transform: scaleY(0.5);
      }
      to {
        opacity: 1;
        transform: scaleY(1);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .message-item.keyboard-focused::before {
        animation: none;
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
      if (!el) return;
      if (this._smoothScrollRaf) cancelAnimationFrame(this._smoothScrollRaf);
      const target = el.scrollHeight - el.clientHeight;
      const start = el.scrollTop;
      const distance = target - start;
      if (Math.abs(distance) < 2) {
        el.scrollTop = target;
        return;
      }
      const startTime = performance.now();
      const duration = Math.min(300, Math.max(80, Math.abs(distance) * 0.8));
      const step = (now: number): void => {
        const elapsed = now - startTime;
        const progress = Math.min(1, elapsed / duration);
        const ease = 1 - Math.pow(1 - progress, 3);
        el.scrollTop = start + distance * ease;
        if (progress < 1) {
          this._smoothScrollRaf = requestAnimationFrame(step);
        } else {
          this._smoothScrollRaf = 0;
        }
      };
      this._smoothScrollRaf = requestAnimationFrame(step);
    });
  }

  scrollToItem(idx: number): void {
    this.updateComplete.then(() => {
      const el = this.scrollContainer?.querySelector(`#msg-${idx}`) as HTMLElement;
      el?.scrollIntoView({ block: "nearest", behavior: "smooth" });
    });
  }

  scrollToMessageId(id: string): void {
    const idx = this.items.findIndex(
      (i) => i.type === "message" && (i as { id?: string }).id === id,
    );
    if (idx >= 0) this.scrollToItem(idx);
  }

  clearKeyboardFocus(): void {
    this._focusedMessageIndex = -1;
  }

  private _getMessageIndices(): number[] {
    const indices: number[] = [];
    for (let i = 0; i < this.items.length; i++) {
      if (this.items[i].type === "message") indices.push(i);
    }
    return indices;
  }

  private _onThreadClick(e: MouseEvent): void {
    const t = e.target as HTMLElement;
    if (t.closest("a, button, input, textarea, [contenteditable]")) return;
    this.scrollContainer?.focus();
  }

  private _onKeydown(e: KeyboardEvent): void {
    const target = e.target as Node;
    if (
      target instanceof HTMLInputElement ||
      target instanceof HTMLTextAreaElement ||
      (target instanceof HTMLElement && target.isContentEditable)
    )
      return;
    const indices = this._getMessageIndices();
    if (indices.length === 0) return;
    const idxPos = indices.indexOf(this._focusedMessageIndex);
    let nextIdx = -1;
    switch (e.key) {
      case "ArrowUp":
        e.preventDefault();
        if (idxPos <= 0) nextIdx = indices[0];
        else nextIdx = indices[idxPos - 1];
        break;
      case "ArrowDown":
        e.preventDefault();
        if (idxPos < 0) nextIdx = indices[0];
        else if (idxPos >= indices.length - 1) nextIdx = indices[indices.length - 1];
        else nextIdx = indices[idxPos + 1];
        break;
      case "Home":
        e.preventDefault();
        nextIdx = indices[0];
        break;
      case "End":
        e.preventDefault();
        nextIdx = indices[indices.length - 1];
        break;
      case "E":
        if (this._focusedMessageIndex >= 0 && !e.metaKey && !e.ctrlKey && !e.altKey) {
          const item = this.items[this._focusedMessageIndex];
          if (item?.type === "message" && item.role === "user") {
            e.preventDefault();
            this.dispatchEvent(
              new CustomEvent("sc-edit-message", {
                bubbles: true,
                composed: true,
                detail: { index: this._focusedMessageIndex },
              }),
            );
          }
        }
        return;
      case "r":
      case "R":
        if (this._focusedMessageIndex >= 0 && !e.metaKey && !e.ctrlKey && !e.altKey) {
          const item = this.items[this._focusedMessageIndex];
          if (item?.type === "message") {
            e.preventDefault();
            this.dispatchEvent(
              new CustomEvent("sc-reply-message", {
                bubbles: true,
                composed: true,
                detail: { index: this._focusedMessageIndex, content: item.content, item },
              }),
            );
          }
        }
        return;
      case "c":
      case "C":
        if (this._focusedMessageIndex >= 0 && !e.metaKey && !e.ctrlKey && !e.altKey) {
          const item = this.items[this._focusedMessageIndex];
          if (item?.type === "message") {
            e.preventDefault();
            navigator.clipboard
              ?.writeText(item.content)
              .then(() => {
                this.dispatchEvent(
                  new CustomEvent("sc-copy-message", {
                    bubbles: true,
                    composed: true,
                    detail: { index: this._focusedMessageIndex },
                  }),
                );
              })
              .catch(() => {});
          }
        }
        return;
      default:
        return;
    }
    if (nextIdx >= 0) {
      this._focusedMessageIndex = nextIdx;
      this.scrollToItem(nextIdx);
    }
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
  private _toggleReaction(idx: number, value: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-toggle-reaction", {
        bubbles: true,
        composed: true,
        detail: { index: idx, value },
      }),
    );
  }

  private _onLoadEarlier(): void {
    this.dispatchEvent(new CustomEvent("sc-load-earlier", { bubbles: true, composed: true }));
  }

  private _getSwipeDelta(rawDelta: number): number {
    const abs = Math.abs(rawDelta);
    if (abs <= SWIPE_START_THRESHOLD) return rawDelta;
    const sign = rawDelta > 0 ? 1 : -1;
    const excess = abs - SWIPE_START_THRESHOLD;
    return sign * (SWIPE_START_THRESHOLD + excess * SWIPE_RESISTANCE);
  }

  private _onSwipePointerDown(e: PointerEvent, idx: number): void {
    if (e.button !== 0 || this._swipeState) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    this._swipeState = { idx, startX: e.clientX, startY: e.clientY, currentX: e.clientX };
    (e.currentTarget as HTMLElement).classList.add("swiping");
  }

  private _onSwipePointerMove(e: PointerEvent, idx: number): void {
    if (!this._swipeState || this._swipeState.idx !== idx) return;
    this._swipeState.currentX = e.clientX;
    const rawDelta = e.clientX - this._swipeState.startX;
    const delta = this._getSwipeDelta(rawDelta);
    const wrapper = this.scrollContainer?.querySelector(`#msg-${idx}`);
    const content = wrapper?.querySelector(".swipe-content") as HTMLElement;
    if (content) content.style.transform = `translateX(${delta}px)`;
  }

  private _onSwipePointerUp(
    e: PointerEvent,
    idx: number,
    item: Extract<ChatItem, { type: "message" }>,
  ): void {
    if (!this._swipeState || this._swipeState.idx !== idx) return;
    (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
    (e.currentTarget as HTMLElement).classList.remove("swiping");
    const rawDelta = this._swipeState.currentX - this._swipeState.startX;
    this._swipeState = null;
    const wrapper = this.scrollContainer?.querySelector(`#msg-${idx}`);
    const content = wrapper?.querySelector(".swipe-content") as HTMLElement;
    if (content) {
      content.style.transform = "";
      if (Math.abs(rawDelta) >= SWIPE_ACTION_THRESHOLD) {
        if (rawDelta > 0) {
          this.dispatchEvent(
            new CustomEvent("sc-swipe-reply", {
              bubbles: true,
              composed: true,
              detail: { index: idx, content: item.content },
            }),
          );
        } else {
          this.dispatchEvent(
            new CustomEvent("sc-swipe-copy", {
              bubbles: true,
              composed: true,
              detail: { index: idx, content: item.content },
            }),
          );
        }
      }
    }
  }

  private async _onOpenImage(e: CustomEvent<{ src: string }>): Promise<void> {
    await import("./sc-image-viewer.js");
    this._imageViewerSrc = e.detail.src;
    this._imageViewerOpen = true;
  }

  private _onCloseImageViewer(): void {
    this._imageViewerOpen = false;
    this._imageViewerSrc = "";
  }

  private _onScrollToMessage(e: CustomEvent<{ id: string }>): void {
    this.scrollToMessageId(e.detail.id);
  }

  private _renderMessageGroup(
    block: Extract<Block, { type: "message-group" }>,
  ): ReturnType<typeof html> {
    const { role, messages, lastTs } = block;
    const lastAssistantIdx = this._findLastAssistantIdx();
    const messageIndices = this._getMessageIndices();
    const messageTotal = messageIndices.length;
    return html`
      <sc-message-group role=${role}>
        ${messages.map(({ item, idx }, i) => {
          const isStreaming =
            this.isWaiting && item.role === "assistant" && idx === lastAssistantIdx;
          const isCompletingBubble =
            this.isCompleting && item.role === "assistant" && idx === lastAssistantIdx;
          const ordinal = messageIndices.indexOf(idx) + 1;
          const isKeyboardFocused = this._focusedMessageIndex === idx;
          return html`
            <div
              class="message-wrapper message-item ${isKeyboardFocused ? "keyboard-focused" : ""}"
              id="msg-${idx}"
              @pointerdown=${(ev: PointerEvent) => this._onSwipePointerDown(ev, idx)}
              @pointermove=${(ev: PointerEvent) => this._onSwipePointerMove(ev, idx)}
              @pointerup=${(ev: PointerEvent) => this._onSwipePointerUp(ev, idx, item)}
              @pointercancel=${(ev: PointerEvent) => this._onSwipePointerUp(ev, idx, item)}
            >
              <div class="swipe-action left" aria-hidden="true">Reply</div>
              <div class="swipe-action right" aria-hidden="true">Copy</div>
              <div class="swipe-content">
                <div class="bubble-wrapper">
                  <sc-message-actions
                    .role=${item.role}
                    .content=${item.content}
                    .index=${idx}
                  ></sc-message-actions>
                  <sc-chat-bubble
                    .content=${item.content}
                    .role=${item.role}
                    .replyTo=${item.replyTo ?? null}
                    .streaming=${isStreaming}
                    .completing=${isCompletingBubble}
                    .showTail=${i === messages.length - 1}
                    .isFirst=${i === 0}
                    .isLast=${i === messages.length - 1}
                    .ariaMessageOrdinal=${ordinal}
                    .ariaMessageTotal=${messageTotal}
                    @contextmenu=${(ev: MouseEvent) => this._onContextMenu(ev, item)}
                  >
                    ${item.role === "user" && item.status
                      ? html`<sc-delivery-status
                          slot="status"
                          .status=${item.status}
                        ></sc-delivery-status>`
                      : nothing}
                    ${item.ts != null
                      ? html`<span slot="meta">${formatTime(item.ts)}</span>`
                      : nothing}
                  </sc-chat-bubble>
                  ${item.reactions?.length
                    ? html`
                        <div class="reaction-pills">
                          ${item.reactions.map(
                            (r: { value: string; count: number; mine: boolean }, ri: number) => {
                              const iconKey = VALUE_TO_ICON[r.value];
                              const icon = iconKey ? icons[iconKey] : null;
                              return html`
                                <button
                                  class="reaction-pill ${r.mine ? "mine" : ""}"
                                  data-stagger="${Math.min(ri, 3)}"
                                  @click=${() => this._toggleReaction(idx, r.value)}
                                  aria-label="${r.value} ${r.count}"
                                >
                                  ${icon
                                    ? html`<span class="reaction-icon">${icon}</span>`
                                    : html`<span class="reaction-fallback">${r.value}</span>`}
                                  <span class="reaction-count">${r.count}</span>
                                </button>
                              `;
                            },
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
                  ${item.role === "assistant" &&
                  item.id &&
                  this.artifacts.some((a) => a.messageId === item.id)
                    ? html`
                        <div class="artifact-cards">
                          ${this.artifacts
                            .filter((a) => a.messageId === item.id)
                            .map(
                              (a) => html`
                                <button
                                  type="button"
                                  class="artifact-card"
                                  @click=${() =>
                                    this.dispatchEvent(
                                      new CustomEvent("open-artifact", {
                                        detail: { id: a.id },
                                        bubbles: true,
                                        composed: true,
                                      }),
                                    )}
                                  aria-label=${`Open artifact: ${a.title}`}
                                >
                                  <span class="artifact-icon"
                                    >${icons[ARTIFACT_TYPE_ICON[a.type]] ??
                                    icons["file-text"]}</span
                                  >
                                  <span class="artifact-title">${a.title}</span>
                                </button>
                              `,
                            )}
                        </div>
                      `
                    : nothing}
                </div>
              </div>
            </div>
          `;
        })}
        ${role === "assistant"
          ? html`<span slot="avatar">${icons["chat-circle"]}</span>`
          : html`<span slot="avatar">${icons.user}</span>`}
        <span slot="timestamp">${formatTime(lastTs)}</span>
      </sc-message-group>
    `;
  }

  private _onHeroChipClick(label: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-hero-suggestion", {
        bubbles: true,
        composed: true,
        detail: { text: label },
      }),
    );
  }

  private _onSuggestionClick(text: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-suggestion-click", {
        bubbles: true,
        composed: true,
        detail: { text },
      }),
    );
  }

  override render() {
    const blocks = this._buildBlocks();
    const isEmpty = this.items.length === 0 && !this.historyLoading && !this.isWaiting;
    return html`
      <div
        id="scroll-container"
        class="messages"
        role="log"
        aria-live="polite"
        aria-label="Chat messages"
        tabindex="0"
        @open-image=${this._onOpenImage}
        @keydown=${this._onKeydown}
        @click=${this._onThreadClick}
      >
        ${this.historyLoading
          ? html`
              <div class="history-skeleton">
                <sc-skeleton variant="card" height="60px"></sc-skeleton>
                <sc-skeleton variant="card" height="60px"></sc-skeleton>
                <sc-skeleton variant="card" height="60px"></sc-skeleton>
              </div>
            `
          : isEmpty
            ? html`
                <div class="hero">
                  <div>
                    <div class="hero-greeting">${getTimeGreeting()}</div>
                    <div class="hero-sub">What would you like to work on?</div>
                  </div>
                  <div class="hero-suggestions">
                    ${HERO_SUGGESTIONS.map(
                      (s) => html`
                        <button
                          class="hero-chip"
                          type="button"
                          @click=${() => this._onHeroChipClick(s.label)}
                        >
                          ${icons[s.icon] ?? nothing}
                          <span>${s.label}</span>
                        </button>
                      `,
                    )}
                  </div>
                </div>
              `
            : html`
                ${this.hasEarlierMessages
                  ? html`
                      <div class="pull-to-load" aria-hidden="true"></div>
                      <div class="load-earlier">
                        ${this.loadingEarlier
                          ? html`<sc-skeleton variant="card" height="40px"></sc-skeleton>`
                          : html`<button
                              class="load-earlier-btn"
                              @click=${this._onLoadEarlier}
                              aria-label="Load earlier messages"
                            >
                              Load earlier messages
                            </button>`}
                      </div>
                    `
                  : nothing}
                ${blocks.map((block) => {
                  if (block.type === "time-divider")
                    return html`<div class="time-divider">
                      <span>${formatTimestampForDivider(block.ts)}</span>
                    </div>`;
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
                ${!this.isWaiting &&
                this._findLastAssistantIdx() >= 0 &&
                (this.suggestions.length > 0 || DEMO_FOLLOWUP_SUGGESTIONS.length > 0)
                  ? html`
                      <div class="suggestions">
                        ${(this.suggestions.length > 0
                          ? this.suggestions
                          : DEMO_FOLLOWUP_SUGGESTIONS
                        ).map(
                          (s, i) => html`
                            <button
                              class="suggestion-chip"
                              data-stagger="${Math.min(i, 2)}"
                              type="button"
                              @click=${() => this._onSuggestionClick(s)}
                            >
                              ${s}
                            </button>
                          `,
                        )}
                      </div>
                    `
                  : nothing}
                ${this.isWaiting
                  ? html`<sc-typing-indicator .elapsed=${this.streamElapsed}></sc-typing-indicator>`
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
      <sc-image-viewer
        .src=${this._imageViewerSrc}
        .open=${this._imageViewerOpen}
        @close=${this._onCloseImageViewer}
      ></sc-image-viewer>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-message-thread": ScMessageThread;
  }
}
