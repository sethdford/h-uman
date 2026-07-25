import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import "./hu-chat-bubble.js";
import "./hu-message-group.js";
import "./hu-typing-indicator.js";
import "./hu-delivery-status.js";
import "./hu-tool-result.js";
import "./hu-reasoning-block.js";
import "./hu-memory-event.js";
import "./hu-web-search-result.js";
import "./hu-skeleton.js";
import "./hu-message-actions.js";
import type { ChatItem, ArtifactData } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import { formatTime, formatTimestampForDivider } from "../utils.js";

const FIVE_MIN_MS = 5 * 60 * 1000;
const SWIPE_START_THRESHOLD = 10;
const SWIPE_ACTION_THRESHOLD = 60;
const SWIPE_RESISTANCE = 0.6;
const SCROLL_AT_BOTTOM_PX = 80;
const SCROLL_AUTO_FOLLOW_PX = 80;

function getTimeGreeting(): string {
  const hour = new Date().getHours();
  if (hour < 5) return "Night owl session";
  if (hour < 12) return "Good morning";
  if (hour < 17) return "Good afternoon";
  if (hour < 21) return "Good evening";
  return "Evening";
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
  | { type: "thinking"; item: Extract<ChatItem, { type: "thinking" }>; idx: number }
  | { type: "memory"; item: Extract<ChatItem, { type: "memory" }>; idx: number }
  | { type: "web_search"; item: Extract<ChatItem, { type: "web_search" }>; idx: number };

@customElement("hu-message-thread")
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
  @state() private _unseenCount = 0;
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

  private _scrollHandler = (): void => {
    const el = this.scrollContainer;
    if (!el) return;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < SCROLL_AT_BOTTOM_PX;
    if (this.showScrollPill === atBottom) this.showScrollPill = !atBottom;
    if (atBottom) this._unseenCount = 0;
  };

  static override styles = css`
    :host {
      display: flex;
      width: 100%;
      min-width: 0;
      flex-direction: column;
      flex: 1;
      contain: layout style;
      container-type: inline-size;
      position: relative;
      min-height: 0;
    }
    .messages {
      flex: 1;
      overflow-y: auto;
      padding: var(--hu-space-md);
      padding-bottom: var(--hu-messages-padding-bottom, var(--hu-space-3xl));
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
      scroll-behavior: smooth;
      overscroll-behavior: contain;
      scroll-padding-top: var(--hu-space-xl);
      scrollbar-width: thin;
      scrollbar-color: var(--hu-border-subtle) transparent;
    }
    .messages::-webkit-scrollbar {
      width: 6px;
    }
    .messages::-webkit-scrollbar-track {
      background: transparent;
    }
    .messages::-webkit-scrollbar-thumb {
      background: var(--hu-border-subtle);
      border-radius: var(--hu-radius-sm);
    }
    .messages::-webkit-scrollbar-thumb:hover {
      background: var(--hu-border);
    }
    .message-wrapper {
      position: relative;
      overflow: hidden;
      content-visibility: auto;
      contain-intrinsic-size: auto 4rem;
    }
    .swipe-content {
      position: relative;
      z-index: 1;
      transition: transform var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
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
      min-height: 2.75rem;
      height: 2.75rem;
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-accent);
      background: var(--hu-accent-subtle);
      border-radius: var(--hu-radius);
      z-index: 0;
      opacity: 0;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
    }
    .message-wrapper.swiping .swipe-action {
      opacity: 1;
    }
    .swipe-action.left {
      left: var(--hu-space-sm);
    }
    .swipe-action.right {
      right: var(--hu-space-sm);
    }
    .bubble-wrapper {
      position: relative;
      max-width: 42rem;
      animation: message-enter var(--hu-duration-normal, 200ms)
        var(--hu-ease-spring-gentle, ease-out) both;
    }
    .bubble-wrapper.user {
      margin-inline-start: auto;
    }
    .bubble-wrapper.assistant {
      margin-inline-end: auto;
      max-width: 100%;
    }
    .bubble-wrapper:hover hu-message-actions {
      opacity: 1;
      transform: translateY(0);
    }
    @media (hover: none) {
      .bubble-wrapper hu-message-actions {
        opacity: var(--hu-opacity-overlay-heavy);
        transform: translateY(0);
      }
    }
    @keyframes message-enter {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-sm, 8px));
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .bubble-wrapper {
        animation: none;
      }
    }
    .time-divider {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: var(--hu-space-sm);
      margin: var(--hu-space-xs) 0;
    }
    .time-divider::before,
    .time-divider::after {
      content: "";
      flex: 1;
      height: 1px;
      background: linear-gradient(to right, transparent, var(--hu-border-subtle));
    }
    .time-divider::after {
      background: linear-gradient(to left, transparent, var(--hu-border-subtle));
    }
    .time-divider span {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-secondary);
      white-space: nowrap;
    }
    @keyframes hu-pill-bounce {
      0% {
        transform: translateX(-50%) translateY(var(--hu-space-lg)) scale(0.8);
        opacity: 0;
      }
      60% {
        transform: translateX(-50%) translateY(calc(-1 * var(--hu-space-xs))) scale(1.05);
        opacity: 1;
      }
      100% {
        transform: translateX(-50%) translateY(0) scale(1);
        opacity: 1;
      }
    }
    .scroll-bottom-pill {
      position: absolute;
      bottom: var(--hu-composer-offset, 1rem);
      left: 50%;
      transform: translateX(-50%);
      background: var(--hu-bg-surface);
      border: 1px solid color-mix(in srgb, var(--hu-border-subtle) 80%, transparent);
      box-shadow: var(--hu-shadow-sm);
      padding: var(--hu-space-xs) var(--hu-space-md);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      z-index: 5;
      animation: hu-pill-bounce var(--hu-duration-slow)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
    }
    @media (prefers-reduced-motion: reduce) {
      .scroll-bottom-pill {
        animation: none;
      }
    }
    .scroll-bottom-pill:hover {
      background: var(--hu-bg-surface);
      border-color: var(--hu-border);
      color: var(--hu-text);
    }
    .scroll-bottom-pill:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .scroll-bottom-pill:active {
      transform: scale(0.95);
    }
    .pill-icon svg {
      width: var(--hu-icon-xs);
      height: var(--hu-icon-xs);
      vertical-align: calc(-1 * var(--hu-space-2xs));
    }
    .waiting-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      align-self: flex-start;
    }
    .abort-btn {
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: transparent;
      color: var(--hu-text-secondary);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      cursor: pointer;
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
    }
    .abort-btn:hover {
      color: var(--hu-error);
      border-color: var(--hu-error);
    }
    .abort-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .abort-btn:active {
      transform: scale(0.97);
    }
    .history-skeleton {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
      padding: var(--hu-space-lg) 0;
    }
    .branch-nav {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      margin-top: var(--hu-space-2xs);
      font-size: var(--hu-text-2xs, 0.625rem);
      color: var(--hu-text-secondary);
      font-family: var(--hu-font);
    }
    .branch-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-md);
      height: var(--hu-icon-md);
      padding: 0;
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      color: var(--hu-text-secondary);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
    }
    .branch-btn:hover {
      color: var(--hu-accent);
      border-color: var(--hu-accent);
    }
    .branch-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .branch-btn:active {
      transform: scale(0.9);
    }
    .branch-btn svg {
      width: var(--hu-icon-xs);
      height: var(--hu-icon-xs);
    }
    .branch-label {
      font-variant-numeric: tabular-nums;
      min-width: var(--hu-space-xl);
      text-align: center;
    }
    .reaction-pills {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-2xs);
      margin-top: var(--hu-space-2xs);
    }
    .reaction-pill {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      cursor: pointer;
      transition: border-color var(--hu-duration-fast) var(--hu-ease-out);
      &:hover {
        border-color: var(--hu-accent);
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      &.mine {
        border-color: var(--hu-accent);
        background: var(--hu-accent-subtle);
      }
    }
    .reaction-icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .reaction-icon svg {
      width: 100%;
      height: 100%;
    }
    .reaction-fallback {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-secondary);
    }
    .reaction-count {
      color: var(--hu-text-secondary);
    }
    @container (max-width: 40rem) /* cq-compact */ {
      .messages {
        padding: var(--hu-space-sm);
      }
      .hero-suggestions {
        grid-template-columns: 1fr;
        gap: var(--hu-space-xs);
      }
      .hero-chip {
        justify-content: center;
      }
      .hero {
        padding: var(--hu-space-xl, 1.5rem);
      }
      .hero-greeting {
        font-size: clamp(1.5rem, 5cqi, var(--hu-text-4xl, 2.5rem));
      }
    }
    .pull-to-load {
      position: sticky;
      top: 0;
      z-index: 2;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: var(--hu-space-sm) 0;
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      transform-origin: center top;
      transition: transform var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
    }
    .pull-to-load::before {
      content: "Pull to load earlier";
    }
    .load-earlier {
      display: flex;
      justify-content: center;
      padding: var(--hu-space-md) 0;
    }
    .load-earlier-btn {
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: transparent;
      color: var(--hu-text-secondary);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }
    .load-earlier-btn:hover {
      color: var(--hu-accent);
      border-color: var(--hu-accent);
    }
    .load-earlier-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .artifact-cards {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      margin-top: var(--hu-space-sm);
    }
    .artifact-card {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-surface-container) 80%, transparent);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      cursor: pointer;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
      &:hover {
        border-color: var(--hu-accent);
        background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
        & .artifact-icon {
          color: var(--hu-accent);
        }
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      & .artifact-icon {
        display: flex;
        align-items: center;
        justify-content: center;
        width: var(--hu-icon-sm);
        height: var(--hu-icon-sm);
        color: var(--hu-text-secondary);
        & svg {
          width: 100%;
          height: 100%;
        }
      }
    }

    /* Empty state hero — centered like claude.ai/ChatGPT */
    .hero {
      flex: 1;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: var(--hu-space-lg);
      padding: var(--hu-space-2xl, 2rem) var(--hu-space-lg);
      padding-bottom: var(--hu-hero-padding-bottom, var(--hu-space-2xl, 2rem));
      text-align: center;
      animation: hu-hero-enter var(--hu-duration-slow) var(--hu-ease-out) both;
    }
    @keyframes hu-hero-enter {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-md));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    .hero-greeting {
      font-family: var(--hu-font);
      font-size: clamp(1.75rem, 4vw, var(--hu-text-4xl, 2.5rem));
      font-weight: var(--hu-weight-semibold, 600);
      letter-spacing: var(--hu-tracking-2xl, -0.02em);
      color: var(--hu-text);
      line-height: var(--hu-leading-tight, 1.25);
    }
    .hero-sub {
      font-size: var(--hu-text-lg);
      color: var(--hu-text-faint);
      margin-top: var(--hu-space-xs);
      letter-spacing: 0.01em;
    }
    .hero-suggestions {
      display: grid;
      grid-template-columns: repeat(2, auto);
      justify-content: center;
      gap: var(--hu-space-sm);
      margin-top: var(--hu-space-lg);
    }
    .hero-chip {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-sm) var(--hu-space-lg);
      background: color-mix(in srgb, var(--hu-surface-container-high) 50%, transparent);
      backdrop-filter: blur(var(--hu-blur-sm, 8px));
      -webkit-backdrop-filter: blur(var(--hu-blur-sm, 8px));
      border: 1px solid color-mix(in srgb, var(--hu-border-subtle) 60%, transparent);
      border-radius: var(--hu-radius-full);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      cursor: pointer;
      animation: hu-chip-enter var(--hu-duration-normal, 300ms) var(--hu-ease-out) both;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
      &:nth-child(1) {
        animation-delay: 200ms;
      }
      &:nth-child(2) {
        animation-delay: 260ms;
      }
      &:nth-child(3) {
        animation-delay: 320ms;
      }
      &:nth-child(4) {
        animation-delay: 380ms;
      }
      &:hover {
        border-color: var(--hu-accent);
        background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
        transform: translateY(calc(-1 * var(--hu-focus-ring-width, 2px)));
      }
      &:active {
        transform: scale(0.97);
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      & svg {
        width: var(--hu-icon-sm);
        height: var(--hu-icon-sm);
        color: var(--hu-accent);
      }
    }
    @keyframes hu-chip-enter {
      from {
        opacity: 0;
        transform: translateY(0.5rem) scale(0.95);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }
    @media (prefers-reduced-transparency: reduce) {
      .hero-chip {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-surface-container);
      }
    }
    /* 3d: Follow-up suggestion chips */
    .suggestions {
      display: flex;
      flex-direction: row;
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) 0;
      align-self: flex-start;
    }
    .suggestion-chip {
      display: inline-flex;
      align-items: center;
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: transparent;
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      cursor: pointer;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
      animation: hu-suggestion-enter var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
      &:hover {
        border-color: var(--hu-accent);
        color: var(--hu-accent);
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      &[data-stagger="1"] {
        animation-delay: var(--hu-stagger-delay, 50ms);
      }
      &[data-stagger="2"] {
        animation-delay: calc(2 * var(--hu-stagger-delay, 50ms));
      }
    }
    @keyframes hu-suggestion-enter {
      from {
        opacity: 0;
        transform: scale(0.95) translateY(var(--hu-space-xs));
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
    @keyframes hu-reaction-pop {
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
      animation: hu-reaction-pop var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }
    .reaction-pill[data-stagger="0"] {
      animation-delay: 0ms;
    }
    .reaction-pill[data-stagger="1"] {
      animation-delay: var(--hu-stagger-delay, 50ms);
    }
    .reaction-pill[data-stagger="2"] {
      animation-delay: calc(2 * var(--hu-stagger-delay, 50ms));
    }
    .reaction-pill[data-stagger="3"] {
      animation-delay: calc(3 * var(--hu-stagger-delay, 50ms));
    }
    .reaction-count {
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
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
      background: var(--hu-accent);
      border-radius: var(--hu-radius-xs);
      animation: hu-focus-bar-enter var(--hu-duration-fast) var(--hu-ease-out) both;
    }
    @keyframes hu-focus-bar-enter {
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
    if (this._smoothScrollRaf) {
      cancelAnimationFrame(this._smoothScrollRaf);
      this._smoothScrollRaf = 0;
    }
    this.scrollContainer?.removeEventListener("scroll", this._scrollHandler);
    super.disconnectedCallback();
  }
  override updated(changed: Map<string, unknown>): void {
    if (changed.has("items") || changed.has("isWaiting")) {
      const el = this.scrollContainer;
      if (!el) return;
      const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < SCROLL_AUTO_FOLLOW_PX;
      if (atBottom) {
        this.scrollToBottom();
      } else if (changed.has("items")) {
        const prev = changed.get("items") as ChatItem[] | undefined;
        const newCount = (this.items?.length ?? 0) - (prev?.length ?? 0);
        if (newCount > 0) this._unseenCount += newCount;
      }
    }
  }

  scrollToBottom(): void {
    this.updateComplete.then(() => {
      const el = this.scrollContainer;
      if (!el) return;
      if (this._smoothScrollRaf) cancelAnimationFrame(this._smoothScrollRaf);
      const target = el.scrollHeight - el.clientHeight + 100;
      const start = el.scrollTop;
      const distance = target - start;
      if (Math.abs(distance) < 2) {
        el.scrollTop = Math.min(el.scrollHeight - el.clientHeight, target);
        return;
      }
      const startTime = performance.now();
      const duration = Math.min(300, Math.max(80, Math.abs(distance) * 0.8));
      const step = (now: number): void => {
        const elapsed = now - startTime;
        const progress = Math.min(1, elapsed / duration);
        const ease = 1 - Math.pow(1 - progress, 3);
        el.scrollTop = Math.min(el.scrollHeight - el.clientHeight, start + distance * ease);
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
        if (this.scrollContainer) this.scrollContainer.scrollTop = 0;
        return;
      case "End":
        e.preventDefault();
        this.scrollToBottom();
        this._unseenCount = 0;
        return;
      case "E":
        if (this._focusedMessageIndex >= 0 && !e.metaKey && !e.ctrlKey && !e.altKey) {
          const item = this.items[this._focusedMessageIndex];
          if (item?.type === "message" && item.role === "user") {
            e.preventDefault();
            this.dispatchEvent(
              new CustomEvent("hu-edit-message", {
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
              new CustomEvent("hu-reply-message", {
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
                  new CustomEvent("hu-copy-message", {
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
      } else if (item.type === "memory") {
        flushGroup();
        blocks.push({ type: "memory", item, idx: i });
      } else if (item.type === "web_search") {
        flushGroup();
        blocks.push({ type: "web_search", item, idx: i });
      }
    }
    flushGroup();
    return blocks;
  }

  private _onContextMenu(ev: MouseEvent, item: ChatItem): void {
    ev.preventDefault();
    this.dispatchEvent(
      new CustomEvent("hu-context-menu", {
        bubbles: true,
        composed: true,
        detail: { event: ev, item },
      }),
    );
  }
  private _onAbort(): void {
    this.dispatchEvent(new CustomEvent("hu-abort", { bubbles: true, composed: true }));
  }
  private _navigateBranch(idx: number, direction: number): void {
    this.dispatchEvent(
      new CustomEvent("hu-branch-navigate", {
        bubbles: true,
        composed: true,
        detail: { index: idx, direction },
      }),
    );
  }
  private _toggleReaction(idx: number, value: string): void {
    this.dispatchEvent(
      new CustomEvent("hu-toggle-reaction", {
        bubbles: true,
        composed: true,
        detail: { index: idx, value },
      }),
    );
  }

  private _onLoadEarlier(): void {
    this.dispatchEvent(new CustomEvent("hu-load-earlier", { bubbles: true, composed: true }));
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
            new CustomEvent("hu-swipe-reply", {
              bubbles: true,
              composed: true,
              detail: { index: idx, content: item.content },
            }),
          );
        } else {
          this.dispatchEvent(
            new CustomEvent("hu-swipe-copy", {
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
    await import("./hu-image-viewer.js");
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
      <hu-message-group role=${role}>
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
                <div class="bubble-wrapper ${item.role}">
                  <hu-message-actions
                    .role=${item.role}
                    .content=${item.content}
                    .index=${idx}
                  ></hu-message-actions>
                  <hu-chat-bubble
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
                    ${
                      item.role === "user" && item.status
                        ? html`<hu-delivery-status
                            slot="status"
                            .status=${item.status}
                          ></hu-delivery-status>`
                        : nothing
                    }
                    ${
                      item.ts != null
                        ? html`<span slot="meta">${formatTime(item.ts)}</span>`
                        : nothing
                    }
                  </hu-chat-bubble>
                  ${
                    item.reactions?.length
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
                                    ${
                                      icon
                                        ? html`<span class="reaction-icon">${icon}</span>`
                                        : html`<span class="reaction-fallback">${r.value}</span>`
                                    }
                                    <span class="reaction-count">${r.count}</span>
                                  </button>
                                `;
                              },
                            )}
                          </div>
                        `
                      : nothing
                  }
                  ${
                    item.branchCount != null && item.branchCount > 1
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
                      : nothing
                  }
                  ${
                    item.role === "assistant" &&
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
                                      >${
                                        icons[ARTIFACT_TYPE_ICON[a.type]] ?? icons["file-text"]
                                      }</span
                                    >
                                    <span class="artifact-title">${a.title}</span>
                                  </button>
                                `,
                              )}
                          </div>
                        `
                      : nothing
                  }
                </div>
              </div>
            </div>
          `;
        })}
        ${
          role === "assistant"
            ? html`<span slot="avatar">${icons["chat-circle"]}</span>`
            : html`<span slot="avatar">${icons.user}</span>`
        }
        <span slot="timestamp">${formatTime(lastTs)}</span>
      </hu-message-group>
    `;
  }

  private _onHeroChipClick(label: string): void {
    this.dispatchEvent(
      new CustomEvent("hu-hero-suggestion", {
        bubbles: true,
        composed: true,
        detail: { text: label },
      }),
    );
  }

  private _onSuggestionClick(text: string): void {
    this.dispatchEvent(
      new CustomEvent("hu-suggestion-click", {
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
        ${
          this.historyLoading
            ? html`
                <div class="history-skeleton">
                  <hu-skeleton variant="card" height="60px"></hu-skeleton>
                  <hu-skeleton variant="card" height="60px"></hu-skeleton>
                  <hu-skeleton variant="card" height="60px"></hu-skeleton>
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
                  ${
                    this.hasEarlierMessages
                      ? html`
                          <div class="pull-to-load" aria-hidden="true"></div>
                          <div class="load-earlier">
                            ${
                              this.loadingEarlier
                                ? html`<hu-skeleton variant="card" height="40px"></hu-skeleton>`
                                : html`<button
                                    class="load-earlier-btn"
                                    @click=${this._onLoadEarlier}
                                    aria-label="Load earlier messages"
                                  >
                                    Load earlier messages
                                  </button>`
                            }
                          </div>
                        `
                      : nothing
                  }
                  ${blocks.map((block) => {
                    if (block.type === "time-divider")
                      return html`<div class="time-divider">
                        <span>${formatTimestampForDivider(block.ts)}</span>
                      </div>`;
                    if (block.type === "message-group") return this._renderMessageGroup(block);
                    if (block.type === "tool_call")
                      return html`<hu-tool-result
                        .tool=${block.item.name}
                        .status=${
                          block.item.status === "completed"
                            ? block.item.result?.startsWith("Error")
                              ? "error"
                              : "success"
                            : "running"
                        }
                        .input=${block.item.input ?? ""}
                        .content=${block.item.result ?? ""}
                      ></hu-tool-result>`;
                    if (block.type === "thinking")
                      return html`<hu-reasoning-block
                        .content=${block.item.content}
                        .streaming=${block.item.streaming}
                        .duration=${block.item.duration ?? ""}
                      ></hu-reasoning-block>`;
                    if (block.type === "memory")
                      return html`<hu-memory-event
                        .action=${block.item.action}
                        .key=${block.item.key}
                        .value=${block.item.value ?? ""}
                      ></hu-memory-event>`;
                    if (block.type === "web_search")
                      return html`<hu-web-search-result
                        .query=${block.item.query}
                        .sites=${block.item.sites}
                        .sources=${block.item.sources ?? []}
                      ></hu-web-search-result>`;
                    return nothing;
                  })}
                  ${
                    !this.isWaiting &&
                    this._findLastAssistantIdx() >= 0 &&
                    this.suggestions.length > 0
                      ? html`
                          <div class="suggestions">
                            ${this.suggestions.map(
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
                      : nothing
                  }
                  ${
                    this.isWaiting
                      ? html`<hu-typing-indicator
                          .elapsed=${this.streamElapsed}
                        ></hu-typing-indicator>`
                      : nothing
                  }
                `
        }
      </div>
      ${
        this.showScrollPill
          ? html`
              <button
                class="scroll-bottom-pill"
                @click=${() => {
                  this.scrollToBottom();
                  this._unseenCount = 0;
                }}
                aria-label=${
                  this._unseenCount > 0
                    ? `${this._unseenCount} new message${this._unseenCount > 1 ? "s" : ""}`
                    : "Scroll to latest message"
                }
              >
                ${
                  this._unseenCount > 0
                    ? html`${this._unseenCount} new message${this._unseenCount > 1 ? "s" : ""} ↓`
                    : html`<span class="pill-icon">${icons["arrow-down"]}</span> New messages`
                }
              </button>
            `
          : nothing
      }
      <hu-image-viewer
        .src=${this._imageViewerSrc}
        .open=${this._imageViewerOpen}
        @close=${this._onCloseImageViewer}
      ></hu-image-viewer>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-message-thread": ScMessageThread;
  }
}
