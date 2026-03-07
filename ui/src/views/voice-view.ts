import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { GatewayClient } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { SESSION_KEY_VOICE, formatRelative } from "../utils.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-button.js";
import "../components/sc-skeleton.js";
import "../components/sc-message-stream.js";
import "../components/sc-thinking.js";

type VoiceStatus = "idle" | "listening" | "processing" | "unsupported";

interface VoiceMessage {
  role: "user" | "assistant";
  content: string;
  ts: number;
}

@customElement("sc-voice-view")
export class ScVoiceView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 720px;
      margin: 0 auto;
      padding: var(--sc-space-lg) var(--sc-space-xl);
    }

    /* ── Hero zone ────────────────────────────────────── */

    .hero {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      padding: var(--sc-space-xl) var(--sc-space-2xl);
      margin-bottom: var(--sc-space-2xl, 2rem);
      background-image: var(--sc-hero-gradient);
      border-radius: var(--sc-radius-xl, 16px);
      border: 1px solid var(--sc-border-subtle, var(--sc-border));
    }

    .hero-left {
      display: flex;
      align-items: center;
      gap: var(--sc-space-lg);
      min-width: 0;
    }

    .hero-title {
      margin: 0;
      font-size: clamp(1.5rem, 2.5vw, 2rem);
      font-weight: var(--sc-weight-bold, 700);
      letter-spacing: -0.03em;
      color: var(--sc-text);
      line-height: 1.1;
    }

    .hero-meta {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .status-dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      flex-shrink: 0;
    }

    .status-dot.connected {
      background: var(--sc-success);
      box-shadow: 0 0 6px var(--sc-success);
    }

    .status-dot.disconnected {
      background: var(--sc-error);
    }

    .status-dot.connecting {
      background: var(--sc-warning);
      animation: sc-status-pulse 2s ease-in-out infinite;
    }

    @keyframes sc-status-pulse {
      0%,
      100% {
        box-shadow: 0 0 4px var(--sc-warning);
      }
      50% {
        box-shadow: 0 0 12px var(--sc-warning);
      }
    }

    .hero-actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }

    .staleness {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    /* ── Voice zone ───────────────────────────────────── */

    .voice-zone {
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: var(--sc-space-2xl) 0 calc(var(--sc-space-2xl, 2rem) * 1.5);
      position: relative;
    }

    .mic-orb-glow {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -60%);
      width: 240px;
      height: 240px;
      border-radius: 50%;
      background: radial-gradient(circle, var(--sc-accent-subtle) 0%, transparent 70%);
      opacity: 0.4;
      pointer-events: none;
      transition: opacity var(--sc-duration-normal) var(--sc-ease-out);
    }

    .mic-orb-glow.active {
      opacity: 0.8;
      animation: sc-glow-breathe 3s ease-in-out infinite;
    }

    @keyframes sc-glow-breathe {
      0%,
      100% {
        opacity: 0.6;
        transform: translate(-50%, -60%) scale(1);
      }
      50% {
        opacity: 1;
        transform: translate(-50%, -60%) scale(1.1);
      }
    }

    .mic-btn-wrap {
      position: relative;
      z-index: 1;
    }

    .mic-btn {
      width: 96px;
      height: 96px;
      border-radius: 50%;
      border: 2px solid var(--sc-border);
      background: var(--sc-bg-surface);
      background-image: linear-gradient(
        180deg,
        rgba(255, 255, 255, 0.08) 0%,
        rgba(0, 0, 0, 0.04) 100%
      );
      color: var(--sc-text-muted);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      position: relative;
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 rgba(255, 255, 255, 0.15),
        inset 0 -1px 0 rgba(0, 0, 0, 0.06);
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
    }

    .mic-btn svg {
      width: 2.5rem;
      height: 2.5rem;
      filter: drop-shadow(0 1px 1px rgba(0, 0, 0, 0.1));
    }

    .mic-btn:hover:not(:disabled) {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      color: var(--sc-accent-text, var(--sc-accent));
      box-shadow:
        var(--sc-shadow-md),
        inset 0 1px 0 rgba(255, 255, 255, 0.2),
        inset 0 -1px 0 rgba(0, 0, 0, 0.06);
      transform: translateY(-2px);
    }

    .mic-btn:active:not(:disabled) {
      transform: translateY(1px) scaleY(0.97) scaleX(1.01);
    }

    .mic-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 4px;
    }

    .mic-btn:disabled {
      opacity: var(--sc-opacity-disabled, 0.5);
      cursor: not-allowed;
    }

    .mic-btn.active {
      background: var(--sc-accent);
      background-image: linear-gradient(
        180deg,
        rgba(255, 255, 255, 0.25) 0%,
        rgba(0, 0, 0, 0.12) 100%
      );
      border-color: var(--sc-accent);
      color: var(--sc-on-accent, #fff);
      box-shadow:
        0 4px 20px rgba(122, 182, 72, 0.35),
        inset 0 1px 0 rgba(255, 255, 255, 0.3),
        inset 0 -1px 0 rgba(0, 0, 0, 0.15);
      transform: translateY(-1px);
    }

    .mic-ring {
      position: absolute;
      top: 50%;
      left: 50%;
      width: 96px;
      height: 96px;
      border-radius: 50%;
      border: 2px solid var(--sc-accent);
      transform: translate(-50%, -50%) scale(1);
      opacity: 0;
      pointer-events: none;
    }

    .mic-ring.active {
      animation: sc-ring-expand 2s ease-out infinite;
    }

    .mic-ring.ring-2.active {
      animation-delay: 0.6s;
    }

    .mic-ring.ring-3.active {
      animation-delay: 1.2s;
    }

    @keyframes sc-ring-expand {
      0% {
        transform: translate(-50%, -50%) scale(1);
        opacity: 0.6;
      }
      100% {
        transform: translate(-50%, -50%) scale(2.2);
        opacity: 0;
      }
    }

    .voice-status {
      margin-top: var(--sc-space-lg);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      position: relative;
      z-index: 1;
    }

    .voice-status.listening,
    .voice-status.processing {
      color: var(--sc-accent-text, var(--sc-accent));
      font-weight: var(--sc-weight-medium);
    }

    /* ── Input bar ────────────────────────────────────── */

    .input-bar {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: flex-end;
      padding: var(--sc-space-md);
      background: var(--sc-bg-surface);
      background-image: var(
        --sc-surface-gradient,
        linear-gradient(180deg, rgba(255, 255, 255, 0.04) 0%, transparent 100%)
      );
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      margin-bottom: var(--sc-space-xl);
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 rgba(255, 255, 255, 0.08);
      backdrop-filter: blur(var(--sc-glass-subtle-blur, 12px));
      -webkit-backdrop-filter: blur(var(--sc-glass-subtle-blur, 12px));
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
      box-sizing: border-box;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }

    .input-bar textarea:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }

    .input-bar textarea::placeholder {
      color: var(--sc-text-muted);
    }

    .send-btn {
      padding: var(--sc-space-sm) var(--sc-space-lg);
      min-height: 44px;
      background: var(--sc-accent);
      background-image: linear-gradient(
        180deg,
        rgba(255, 255, 255, 0.2) 0%,
        rgba(0, 0, 0, 0.1) 100%
      );
      color: var(--sc-on-accent, var(--sc-bg));
      border: none;
      border-radius: var(--sc-radius);
      font-weight: var(--sc-weight-semibold, 600);
      cursor: pointer;
      font-size: var(--sc-text-base);
      box-shadow:
        0 2px 8px rgba(122, 182, 72, 0.2),
        inset 0 1px 0 rgba(255, 255, 255, 0.25),
        inset 0 -1px 0 rgba(0, 0, 0, 0.1);
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
    }

    .send-btn:hover:not(:disabled) {
      background: var(--sc-accent-hover);
      background-image: linear-gradient(
        180deg,
        rgba(255, 255, 255, 0.25) 0%,
        rgba(0, 0, 0, 0.08) 100%
      );
      box-shadow:
        0 4px 16px rgba(122, 182, 72, 0.3),
        inset 0 1px 0 rgba(255, 255, 255, 0.3),
        inset 0 -1px 0 rgba(0, 0, 0, 0.1);
      transform: translateY(-1px);
    }

    .send-btn:active:not(:disabled) {
      transform: translateY(1px) scaleY(0.97) scaleX(1.01);
      box-shadow:
        0 1px 4px rgba(122, 182, 72, 0.2),
        inset 0 1px 0 rgba(255, 255, 255, 0.15);
    }

    .send-btn:disabled {
      opacity: var(--sc-opacity-disabled, 0.5);
      cursor: not-allowed;
    }

    .send-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    /* ── Conversation zone ────────────────────────────── */

    @keyframes sc-slide-up {
      from {
        opacity: 0;
        transform: translateY(8px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .conversation {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      max-height: 420px;
      overflow-y: auto;
      padding: var(--sc-space-lg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      background: var(--sc-bg-surface);
      background-image: var(
        --sc-surface-gradient,
        linear-gradient(180deg, rgba(255, 255, 255, 0.02) 0%, transparent 100%)
      );
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 rgba(255, 255, 255, 0.06);
      scroll-behavior: smooth;
    }

    .msg {
      max-width: 85%;
      padding: var(--sc-space-md) var(--sc-space-lg);
      border-radius: var(--sc-radius-lg);
      font-size: var(--sc-text-base);
      line-height: 1.6;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      animation: sc-slide-up var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    .msg.user {
      align-self: flex-end;
      background: var(--sc-accent);
      background-image: linear-gradient(
        180deg,
        rgba(255, 255, 255, 0.15) 0%,
        rgba(0, 0, 0, 0.08) 100%
      );
      color: var(--sc-on-accent, var(--sc-bg));
      box-shadow:
        0 2px 8px rgba(122, 182, 72, 0.18),
        inset 0 1px 0 rgba(255, 255, 255, 0.15);
      border-bottom-right-radius: var(--sc-radius-sm, 4px);
    }

    .msg.assistant {
      align-self: flex-start;
      background: var(--sc-bg-elevated);
      background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.06) 0%, transparent 100%);
      border: 1px solid var(--sc-border);
      color: var(--sc-text);
      box-shadow:
        var(--sc-shadow-sm, 0 1px 3px rgba(0, 0, 0, 0.05)),
        inset 0 1px 0 rgba(255, 255, 255, 0.08);
      border-bottom-left-radius: var(--sc-radius-sm, 4px);
    }

    .msg-meta {
      font-size: var(--sc-text-xs);
      opacity: var(--sc-opacity-muted, 0.8);
    }

    .msg.user .msg-meta {
      align-self: flex-end;
    }

    .thinking-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      align-self: flex-start;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      font-style: italic;
      animation: sc-slide-up var(--sc-duration-normal) var(--sc-ease-out) both;
    }

    .empty-conversation {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--sc-space-md);
      padding: var(--sc-space-2xl, 2rem) var(--sc-space-lg);
      text-align: center;
    }

    .empty-icon {
      width: 48px;
      height: 48px;
      color: var(--sc-text-muted);
      opacity: 0.3;
    }

    .empty-text {
      color: var(--sc-text-muted);
      font-size: var(--sc-text-sm);
      max-width: 280px;
      line-height: 1.6;
    }

    /* ── Skeleton ─────────────────────────────────────── */

    .skeleton-hero {
      height: 90px;
      margin-bottom: var(--sc-space-2xl, 2rem);
      border-radius: var(--sc-radius-xl, 16px);
    }

    .skeleton-mic {
      display: flex;
      justify-content: center;
      margin-bottom: var(--sc-space-xl);
    }

    .skeleton-input {
      margin-bottom: var(--sc-space-xl);
    }

    /* ── Responsive ───────────────────────────────────── */

    @media (max-width: 480px) {
      :host {
        max-width: 100%;
        padding: var(--sc-space-md);
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
      .mic-btn.active,
      .status-dot.connecting,
      .msg {
        animation: none !important;
      }
      .mic-btn.active {
        box-shadow: 0 0 0 6px var(--sc-accent-subtle);
      }
    }
  `;

  @state() private transcript = "";
  @state() private voiceStatus: VoiceStatus = "idle";
  @state() private speechSupported = true;
  @state() private recognition: SpeechRecognition | null = null;
  @state() private _messages: VoiceMessage[] = [];
  @state() private _connectionStatus: GatewayStatus = "disconnected";
  @state() private _loading = true;

  private gatewayHandler = (e: Event): void => this.onGatewayEvent(e);
  private statusHandler = (e: Event): void => {
    this._connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
  };
  private _boundGateway: GatewayClient | null = null;

  private get _cacheKey(): string {
    return `sc-voice-messages`;
  }

  private _cacheMessages(): void {
    try {
      sessionStorage.setItem(this._cacheKey, JSON.stringify(this._messages));
    } catch {
      /* quota exceeded */
    }
  }

  private _restoreFromCache(): void {
    try {
      const raw = sessionStorage.getItem(this._cacheKey);
      if (!raw) return;
      const cached = JSON.parse(raw) as unknown;
      if (Array.isArray(cached)) {
        this._messages = cached.filter(
          (m: unknown) => typeof m === "object" && m !== null && "role" in m && "content" in m,
        ) as VoiceMessage[];
      }
    } catch {
      /* corrupt cache */
    }
  }

  override firstUpdated(): void {
    this.speechSupported = "webkitSpeechRecognition" in window || "SpeechRecognition" in window;
    if (!this.speechSupported) {
      this.voiceStatus = "unsupported";
      ScToast.show({
        message: "Speech recognition is not supported in this browser",
        variant: "info",
      });
    }
    this._restoreFromCache();
    const gw = this.gateway;
    if (gw) {
      this._boundGateway = gw;
      this._connectionStatus = gw.status;
      gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
      gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
    }
    this._loading = false;
  }

  protected override async load(): Promise<void> {
    if (!this._boundGateway && this.gateway) {
      this._boundGateway = this.gateway;
      this._connectionStatus = this.gateway.status;
      this.gateway.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
      this.gateway.addEventListener(
        GatewayClientClass.EVENT_STATUS,
        this.statusHandler as EventListener,
      );
    }
  }

  override disconnectedCallback(): void {
    this._boundGateway?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
    this._boundGateway?.removeEventListener(
      GatewayClientClass.EVENT_STATUS,
      this.statusHandler as EventListener,
    );
    this._boundGateway = null;
    this.stopRecognition();
    super.disconnectedCallback();
  }

  private _findLastAssistantIdx(): number {
    for (let i = this._messages.length - 1; i >= 0; i--) {
      if (this._messages[i].role === "assistant") return i;
    }
    return -1;
  }

  private onGatewayEvent(e: Event): void {
    const ev = e as CustomEvent<{
      event: string;
      payload?: Record<string, unknown>;
    }>;
    const detail = ev.detail;
    if (!detail?.event || detail.event !== "chat") return;
    const payload = detail.payload ?? {};
    const sessionKey = (payload.session_key as string) ?? (payload.sessionKey as string);
    if (sessionKey !== SESSION_KEY_VOICE) return;
    const state = payload.state as string;
    const content = (payload.message as string) ?? "";

    if (state === "sent" && content) {
      this._messages = [...this._messages, { role: "assistant", content, ts: Date.now() }];
      this.voiceStatus = "idle";
    }
    if (state === "chunk" && content) {
      const lastIdx = this._findLastAssistantIdx();
      if (lastIdx >= 0 && this._messages[lastIdx].role === "assistant") {
        const last = this._messages[lastIdx];
        this._messages = [
          ...this._messages.slice(0, lastIdx),
          { ...last, content: last.content + content },
          ...this._messages.slice(lastIdx + 1),
        ];
      } else {
        this._messages = [...this._messages, { role: "assistant", content, ts: Date.now() }];
      }
      this.voiceStatus = "processing";
    }
    if (state === "received") {
      this.voiceStatus = "processing";
    }
    this._cacheMessages();
    this.requestUpdate();
    this._scrollConversation();
  }

  private _scrollConversation(): void {
    this.updateComplete.then(() => {
      const el = this.renderRoot.querySelector(".conversation");
      if (el) el.scrollTop = el.scrollHeight;
    });
  }

  private startRecognition(): void {
    if (!this.speechSupported) return;
    const Ctor =
      (
        window as unknown as {
          webkitSpeechRecognition?: new () => SpeechRecognition;
        }
      ).webkitSpeechRecognition ??
      (window as unknown as { SpeechRecognition?: new () => SpeechRecognition }).SpeechRecognition;
    if (!Ctor) return;
    const rec = new Ctor();
    rec.continuous = false;
    rec.interimResults = true;
    rec.lang = "en-US";
    rec.onresult = (event: SpeechRecognitionEvent) => {
      const r = event.results[event.results.length - 1];
      if (!r) return;
      this.transcript = r[0]?.transcript ?? "";
      this.requestUpdate();
    };
    rec.onend = () => {
      if (this.voiceStatus === "listening") this.voiceStatus = "idle";
      this.recognition = null;
      this.requestUpdate();
    };
    rec.onerror = (ev: Event) => {
      this.voiceStatus = "idle";
      this.recognition = null;
      const errCode = (ev as Event & { error?: string }).error ?? "unknown";
      if (errCode !== "aborted" && errCode !== "no-speech") {
        ScToast.show({ message: `Speech recognition error: ${errCode}`, variant: "error" });
      }
      this.requestUpdate();
    };
    rec.start();
    this.recognition = rec;
    this.voiceStatus = "listening";
  }

  private stopRecognition(): void {
    if (this.recognition) {
      try {
        this.recognition.stop();
      } catch {
        /* ignore */
      }
      this.recognition = null;
    }
    if (this.voiceStatus === "listening") this.voiceStatus = "idle";
  }

  private toggleMic(): void {
    if (this.voiceStatus === "listening") {
      this.stopRecognition();
    } else {
      this.startRecognition();
    }
  }

  private async send(): Promise<void> {
    const text = this.transcript.trim();
    const gw = this.gateway;
    if (!text || !gw) return;
    this._messages = [...this._messages, { role: "user", content: text, ts: Date.now() }];
    this.transcript = "";
    this.voiceStatus = "processing";
    this._cacheMessages();
    this._scrollConversation();
    try {
      await gw.request<{ status?: string; sessionKey?: string }>("chat.send", {
        message: text,
        sessionKey: SESSION_KEY_VOICE,
      });
    } catch (err) {
      this.voiceStatus = "idle";
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

  private get statusText(): string {
    switch (this.voiceStatus) {
      case "listening":
        return "Listening\u2026";
      case "processing":
        return "Processing\u2026";
      case "unsupported":
        return "Speech recognition not supported in this browser";
      default:
        return "Click the microphone to start speaking";
    }
  }

  override render() {
    if (this._loading) return this._renderSkeleton();
    return html`
      ${this._renderHero()} ${this._renderVoiceZone()} ${this._renderInputBar()}
      ${this._renderConversation()}
    `;
  }

  private _renderSkeleton() {
    return html`
      <sc-skeleton variant="card" class="skeleton-hero"></sc-skeleton>
      <div class="skeleton-mic">
        <sc-skeleton variant="circle" width="96px" height="96px"></sc-skeleton>
      </div>
      <sc-skeleton variant="card" height="60px" class="skeleton-input"></sc-skeleton>
      <sc-skeleton variant="card" height="200px"></sc-skeleton>
    `;
  }

  private _renderHero() {
    const statusLabel =
      this._connectionStatus === "connected"
        ? "Connected"
        : this._connectionStatus === "connecting"
          ? "Reconnecting\u2026"
          : "Disconnected";
    return html`
      <div class="hero">
        <div class="hero-left">
          <span class="status-dot ${this._connectionStatus}" aria-hidden="true"></span>
          <div>
            <h2 class="hero-title">Voice Assistant</h2>
            <div class="hero-meta">
              <span>${statusLabel}</span>
            </div>
          </div>
        </div>
        <div class="hero-actions">
          <span class="staleness">${this.stalenessLabel}</span>
          <sc-button size="sm" @click=${() => this.load()} aria-label="Refresh data">
            Refresh
          </sc-button>
        </div>
      </div>
    `;
  }

  private _renderVoiceZone() {
    const isActive = this.voiceStatus === "listening";
    return html`
      <div class="voice-zone">
        <div class="mic-orb-glow ${isActive ? "active" : ""}"></div>
        <div class="mic-btn-wrap">
          <div class="mic-ring ${isActive ? "active" : ""}"></div>
          <div class="mic-ring ring-2 ${isActive ? "active" : ""}"></div>
          <div class="mic-ring ring-3 ${isActive ? "active" : ""}"></div>
          <button
            class="mic-btn ${isActive ? "active" : ""}"
            ?disabled=${!this.speechSupported || this._connectionStatus === "disconnected"}
            @click=${this.toggleMic}
            aria-label=${isActive ? "Stop listening" : "Start listening"}
          >
            ${icons.mic}
          </button>
        </div>
        <div class="voice-status ${this.voiceStatus}" aria-live="polite">${this.statusText}</div>
      </div>
    `;
  }

  private _renderInputBar() {
    return html`
      <div class="input-bar">
        <textarea
          placeholder="Speech transcript appears here, or type manually…"
          .value=${this.transcript}
          ?disabled=${this._connectionStatus === "disconnected"}
          @input=${(e: Event) => {
            this.transcript = (e.target as HTMLTextAreaElement).value;
          }}
          @keydown=${this.handleKeyDown}
          aria-label="Speech transcript"
        ></textarea>
        <button
          class="send-btn"
          ?disabled=${!this.transcript.trim() ||
          this.voiceStatus === "processing" ||
          this._connectionStatus === "disconnected"}
          @click=${() => this.send()}
          aria-label="Send voice message"
        >
          Send
        </button>
      </div>
    `;
  }

  private _renderConversation() {
    if (this._messages.length === 0 && this.voiceStatus !== "processing") {
      return html`
        <div class="conversation">
          <div class="empty-conversation">
            <div class="empty-icon">${icons.mic}</div>
            <div class="empty-text">
              Your voice conversation will appear here.<br />
              Tap the microphone or type a message to begin.
            </div>
          </div>
        </div>
      `;
    }

    const lastAssistantIdx = this._findLastAssistantIdx();

    return html`
      <div class="conversation" role="log" aria-live="polite" aria-label="Voice conversation">
        ${this._messages.map((msg, idx) => {
          if (msg.role === "user") {
            return html`
              <div class="msg user">
                <span>${msg.content}</span>
                <span class="msg-meta">${formatRelative(msg.ts)}</span>
              </div>
            `;
          }
          const isStreaming = this.voiceStatus === "processing" && idx === lastAssistantIdx;
          return html`
            <div class="msg assistant">
              <sc-message-stream
                .content=${msg.content}
                .streaming=${isStreaming}
                .role=${"assistant"}
              ></sc-message-stream>
              <span class="msg-meta">${formatRelative(msg.ts)}</span>
            </div>
          `;
        })}
        ${this.voiceStatus === "processing" &&
        (this._messages.length === 0 || this._messages[this._messages.length - 1]?.role === "user")
          ? html`
              <div class="thinking-row">
                <sc-thinking .active=${true} .steps=${[]}></sc-thinking>
              </div>
            `
          : nothing}
      </div>
    `;
  }
}
