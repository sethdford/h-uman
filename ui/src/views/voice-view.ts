import { html, css, nothing } from "lit";
import { customElement, query, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import type { GatewayClient } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { SESSION_KEY_VOICE } from "../utils.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import { AudioRecorder, blobToBase64 } from "../audio-recorder.js";
import { AudioPlaybackEngine } from "../audio-playback.js";
import { createVoiceSilenceController } from "../voice-stream-silence.js";
import type { VoiceOrbState } from "../components/hu-voice-orb.js";
import "../components/hu-button.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-status-dot.js";
import "../components/hu-voice-orb.js";
import "../components/hu-voice-conversation.js";
import "../components/hu-voice-clone.js";
import "../components/hu-input.js";

type VoiceStatus = "idle" | "listening" | "processing" | "unsupported";

interface VoiceMessage {
  role: "user" | "assistant";
  content: string;
  ts: number;
}

@customElement("hu-voice-view")
export class ScVoiceView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  private _scrollEntranceObserver: IntersectionObserver | null = null;

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-voice;
        display: flex;
        width: 100%;
        flex-direction: column;
        height: 100%;
        max-height: calc(100vh - var(--hu-space-5xl));
        max-height: calc(100dvh - var(--hu-space-5xl));
        contain: layout style;
        container-type: inline-size;
      }

      /* ── Sessions panel + main wrap ─────────────────── */

      .main-wrap {
        display: flex;
        flex-direction: row;
        flex: 1;
        min-width: 0;
        position: relative;
        width: 100%;
        min-height: 0;
      }

      .sessions-panel {
        width: 0;
        overflow: hidden;
        flex-shrink: 0;
        transition: width var(--hu-duration-normal) var(--hu-ease-spring);
        contain: layout style;
      }

      .sessions-panel.open {
        width: 17.5rem;
      }

      .sessions-panel-inner {
        width: 17.5rem;
        height: 100%;
        display: flex;
        flex-direction: column;
        background: var(--hu-surface-container);
        border-right: 1px solid var(--hu-border-subtle);
      }

      .sessions-panel-header {
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-bottom: 1px solid var(--hu-border-subtle);
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text);
      }

      .sessions-panel-section {
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-bottom: 1px solid var(--hu-border-subtle);
      }

      .sessions-panel-section h4 {
        margin: 0 0 var(--hu-space-xs);
        font-size: var(--hu-text-xs);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text-secondary);
        text-transform: uppercase;
        letter-spacing: 0.05em;
      }

      .sessions-panel-list {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-2xs);
        overflow-y: auto;
        flex: 1;
        padding: var(--hu-space-sm) var(--hu-space-md);
      }

      .session-item {
        display: flex;
        align-items: center;
        gap: var(--hu-space-sm);
        padding: var(--hu-space-xs) var(--hu-space-sm);
        border-radius: var(--hu-radius-sm);
        border: none;
        background: transparent;
        color: var(--hu-text);
        font-family: var(--hu-font);
        font-size: var(--hu-text-sm);
        cursor: pointer;
        text-align: left;
        width: 100%;
        transition: background var(--hu-duration-fast) var(--hu-ease-out);
      }

      .session-item:hover {
        background: var(--hu-hover-overlay);
      }

      .session-item:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .session-item.active {
        background: color-mix(in srgb, var(--hu-accent) 12%, transparent);
        color: var(--hu-accent);
        font-weight: var(--hu-weight-medium);
      }

      .session-item-icon {
        flex-shrink: 0;
        width: 1rem;
        height: 1rem;
        color: var(--hu-text-secondary);
      }

      .session-item.active .session-item-icon {
        color: var(--hu-accent);
      }

      .session-item-label {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }

      .sessions-panel-empty {
        padding: var(--hu-space-md);
        font-size: var(--hu-text-xs);
        color: var(--hu-text-tertiary);
        text-align: center;
      }

      .sessions-toggle {
        display: flex;
        align-items: center;
        justify-content: center;
        min-width: 2.75rem;
        min-height: 2.75rem;
        padding: var(--hu-space-2xs) var(--hu-space-sm);
        background: transparent;
        border: 1px solid var(--hu-border);
        border-radius: var(--hu-radius-sm);
        color: var(--hu-text);
        cursor: pointer;
        transition:
          color var(--hu-duration-fast) var(--hu-ease-out),
          border-color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .sessions-toggle:hover {
        color: var(--hu-text);
        border-color: var(--hu-text-muted);
      }

      .sessions-toggle:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .sessions-toggle svg {
        width: 1.125rem;
        height: 1.125rem;
      }

      @media (max-width: 904px) /* --hu-breakpoint-medium */ {
        .sessions-panel.open {
          position: fixed;
          left: 0;
          top: 0;
          bottom: 0;
          z-index: 20;
          width: 17.5rem;
        }
      }

      .container {
        display: flex;
        flex-direction: column;
        flex: 1;
        height: 100%;
        max-width: var(--hu-content-width);
        margin: 0 auto;
        position: relative;
        width: 100%;
        min-height: 0;
        border-radius: var(--hu-radius-lg);
        overflow: hidden;
      }

      /* ── Status bar ─────────────────────────────────── */

      .status-bar {
        display: flex;
        align-items: center;
        flex-wrap: wrap;
        gap: var(--hu-space-xs);
        padding: var(--hu-space-xs) var(--hu-space-md);
        font-size: var(--hu-text-xs);
        color: var(--hu-text-secondary);
        background: color-mix(in srgb, var(--hu-surface-container) 60%, transparent);
        backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
          saturate(var(--hu-glass-subtle-saturate, 120%));
        -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
          saturate(var(--hu-glass-subtle-saturate, 120%));
        border-bottom: 1px solid var(--hu-border-subtle);
        flex-shrink: 0;
      }

      .status-left {
        display: flex;
        align-items: center;
        gap: var(--hu-space-sm);
        min-width: 0;
      }

      .status-right {
        display: flex;
        align-items: center;
        gap: var(--hu-space-2xs);
        flex-wrap: wrap;
        margin-left: auto;
      }

      .status-title {
        font-weight: var(--hu-weight-medium);
        color: var(--hu-text);
        font-size: var(--hu-text-sm);
      }

      .status-meta {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-secondary);
      }

      /* ── Error banner ───────────────────────────────── */

      .error-banner {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: var(--hu-space-md);
        background: var(--hu-error-dim);
        border: 1px solid var(--hu-error);
        border-radius: var(--hu-radius);
        color: var(--hu-error);
        font-size: var(--hu-text-base);
        flex-shrink: 0;
      }

      /* ── Conversation area (empty state) ───────────── */

      .conversation-area {
        flex: 1;
        display: flex;
        align-items: center;
        justify-content: center;
        min-height: 0;
      }

      /* ── Controls zone (orb + input bar) ────────────── */

      .controls-zone {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: var(--hu-space-sm);
        padding: var(--hu-space-sm) var(--hu-space-md) var(--hu-space-md);
        flex-shrink: 0;
      }

      .input-row {
        display: flex;
        gap: var(--hu-space-sm);
        align-items: flex-end;
        width: 100%;
        padding: var(--hu-space-md);
        background: var(--hu-surface-container);
        background-image: var(--hu-surface-gradient);
        border: 1px solid var(--hu-border);
        border-radius: var(--hu-radius-lg);
        box-shadow: var(--hu-shadow-card);
        backdrop-filter: blur(var(--hu-glass-subtle-blur));
        -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur));
        box-sizing: border-box;
      }

      .input-row hu-textarea {
        flex: 1;
        --hu-bg-elevated: var(--hu-bg);
      }

      .input-row hu-button {
        min-height: 2.75rem;
      }

      /* ── Setup banner ──────────────────────────────────── */

      .setup-banner {
        background: var(--hu-surface-container);
        border-radius: var(--hu-radius-lg);
        padding: var(--hu-space-md);
        margin: var(--hu-space-sm) var(--hu-space-md);
        flex-shrink: 0;
      }

      .setup-banner fieldset {
        gap: var(--hu-space-sm);
      }

      .provider-option {
        display: flex;
        align-items: center;
        gap: var(--hu-space-2xs);
        cursor: pointer;
        font-size: var(--hu-text-sm);
        font-family: var(--hu-font);
        padding: var(--hu-space-2xs) var(--hu-space-sm);
        border-radius: var(--hu-radius-md);
        border: 1px solid transparent;
        transition:
          border-color var(--hu-duration-fast) var(--hu-ease-out),
          background var(--hu-duration-fast) var(--hu-ease-out);
      }
      .provider-option:hover {
        background: var(--hu-hover-overlay);
      }
      .provider-option[data-selected] {
        border-color: var(--hu-accent);
        background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
      }
      .provider-option input[type="radio"] {
        appearance: none;
        -webkit-appearance: none;
        /* 1.25rem (20px) meets the WCAG 20px hard floor for touch
         * targets — Wave 2 design-standards spec. */
        width: 1.25rem;
        height: 1.25rem;
        border: 2px solid var(--hu-border);
        border-radius: 50%;
        margin: 0;
        flex-shrink: 0;
        position: relative;
        transition: border-color var(--hu-duration-fast) var(--hu-ease-out);
      }
      .provider-option input[type="radio"]:checked {
        border-color: var(--hu-accent);
      }
      .provider-option input[type="radio"]:checked::after {
        content: "";
        position: absolute;
        top: 50%;
        left: 50%;
        transform: translate(-50%, -50%);
        width: 0.5rem;
        height: 0.5rem;
        border-radius: 50%;
        background: var(--hu-accent);
      }

      /* ── Skeleton ────────────────────────────────────── */

      .skeleton-bar {
        height: 2.25rem;
        margin-bottom: var(--hu-space-xs);
      }

      .skeleton-controls {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: var(--hu-space-sm);
        padding: var(--hu-space-md);
      }

      /* ── Responsive ──────────────────────────────────── */

      @container (max-width: 30rem) /* cq-sm */ {
        .input-row {
          flex-direction: column;
          align-items: stretch;
        }
        .input-row hu-button {
          min-height: 2.5rem;
        }
      }
      @media (prefers-reduced-motion: reduce) {
        * {
          animation-duration: 0s !important;
          transition-duration: 0s !important;
        }
      }

      @media (prefers-reduced-transparency: reduce) {
        .status-bar,
        .input-row {
          backdrop-filter: none;
          -webkit-backdrop-filter: none;
          background: var(--hu-surface-container);
        }
      }
    `,
  ];

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => this._setupScrollEntrance());
  }

  @state() private transcript = "";
  @state() private voiceStatus: VoiceStatus = "idle";
  @state() private error = "";
  @state() private _lastFailedMessage = "";
  @state() private _messages: VoiceMessage[] = [];
  @state() private _connectionStatus: GatewayStatus = "disconnected";
  @state() private _loading = true;
  @state() private _sessionStartTs: number | null = null;
  @state() private _sessionDurationSec = 0;
  @state() private _speaking = false;
  @state() private _audioLevel = 0;
  @state() private _showClonePanel = false;
  @state() private _sessionsPanelOpen = false;
  @state() private _geminiLiveMode = false;
  @state() private _userVadSpeaking = false;
  @state() private _voiceMode: "standard" | "gemini_live" | "openai_realtime" = "standard";
  @state() private _stsApiKey = "";
  @state() private _showCredPanel = false;
  @state() private _showSetup = false;
  @state() private _setupMode = "gemini_live";
  @state() private _setupKey = "";
  @state() private _setupValidating = false;
  @state() private _setupError = "";
  @state() private _setupSuccess = false;
  private _durationTimer: ReturnType<typeof setInterval> | null = null;
  private _recorder = new AudioRecorder();
  readonly #playback = new AudioPlaybackEngine(24000);
  #voiceStreaming = false;
  #activeSessionIsGL = false;
  #processingTimer: ReturnType<typeof setTimeout> | null = null;
  readonly #silence = createVoiceSilenceController({
    isActive: () => this.voiceStatus === "listening" && this.#voiceStreaming,
    onSilenceEnd: () => {
      void this._stopRecording();
    },
  });

  private gatewayHandler = (e: Event): void => this.onGatewayEvent(e);
  private statusHandler = (e: Event): void => {
    const prev = this._connectionStatus;
    this._connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
    if (prev !== "disconnected" && this._connectionStatus === "disconnected") {
      void this.#onGatewayDisconnectedDuringVoice();
    }
    if (prev === "disconnected" && this._connectionStatus === "connected") {
      void this.#onGatewayReconnected();
    }
  };
  private _boundGateway: GatewayClient | null = null;

  @query("hu-voice-conversation") private _conversation!: HTMLElement & {
    scrollToBottom: () => void;
  };

  private _scrollConversation(): void {
    this._conversation?.scrollToBottom();
  }

  private get _cacheKey(): string {
    return `hu-voice-messages`;
  }

  private get _orbVisualState(): VoiceOrbState {
    if (this.voiceStatus === "unsupported") return "unsupported";
    if (this.voiceStatus === "listening") return "listening";
    if (this._speaking) return "speaking";
    if (this.voiceStatus === "processing") return "processing";
    return "idle";
  }

  private get _sessionCountKey(): string {
    return `hu-voice-session-count`;
  }

  private get _sessionCount(): number {
    try {
      const raw = sessionStorage.getItem(this._sessionCountKey);
      const n = raw ? parseInt(raw, 10) : 1;
      return Number.isFinite(n) && n >= 1 ? n : 1;
    } catch {
      return 1;
    }
  }

  private _incrementSessionCount(): void {
    try {
      const next = this._sessionCount + 1;
      sessionStorage.setItem(this._sessionCountKey, String(next));
    } catch {
      /* ignore */
    }
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
    requestAnimationFrame(() => {
      if (!this._recorder.isSupported) {
        this.voiceStatus = "unsupported";
        ScToast.show({
          message: "Audio recording is not supported in this browser",
          variant: "info",
        });
      }
      try {
        const savedMode = localStorage.getItem("hu-voice-mode");
        if (savedMode === "gemini_live" || savedMode === "openai_realtime") {
          this._voiceMode = savedMode;
          this._geminiLiveMode = savedMode === "gemini_live";
        } else {
          this._geminiLiveMode = localStorage.getItem("hu-gemini-live") === "true";
          if (this._geminiLiveMode) this._voiceMode = "gemini_live";
        }
        const savedKey = localStorage.getItem("hu-sts-api-key");
        if (savedKey) this._stsApiKey = savedKey;
      } catch {
        /* ignore */
      }
      this._restoreFromCache();
      try {
        this._sessionsPanelOpen = localStorage.getItem("hu-voice-sessions-open") === "true";
      } catch {
        /* ignore */
      }
      this.#playback.setOnPlaybackEnd(() => {
        this._speaking = false;
        if (this.voiceStatus !== "listening") {
          this.voiceStatus = "idle";
        }
        this.requestUpdate();
      });
      const gw = this.gateway;
      if (gw) {
        this._boundGateway = gw;
        this._connectionStatus = gw.status;
        gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
        gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
      }
      this._loading = false;
      /* Show setup banner if no provider key is configured */
      try {
        const hasKey = localStorage.getItem("hu-voice-api-key");
        if (!hasKey) this._showSetup = true;
      } catch {
        /* ignore */
      }
    });
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

  protected override onGatewaySwapped(
    previous: GatewayClientClass | null,
    current: GatewayClientClass,
  ): void {
    void this.#teardownVoiceGateway(previous);
    previous?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
    previous?.removeEventListener(
      GatewayClientClass.EVENT_STATUS,
      this.statusHandler as EventListener,
    );
    this._boundGateway = current;
    this._connectionStatus = current.status;
    current.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
    current.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
  }

  override disconnectedCallback(): void {
    void this.#teardownVoiceGateway(this._boundGateway);
    this.#playback.dispose();
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    this._stopDurationTimer();
    this._boundGateway?.removeEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
    this._boundGateway?.removeEventListener(
      GatewayClientClass.EVENT_STATUS,
      this.statusHandler as EventListener,
    );
    this._boundGateway = null;
    this._recorder.dispose();
    super.disconnectedCallback();
  }

  private _setupScrollEntrance(): void {
    if (typeof CSS !== "undefined" && CSS.supports?.("animation-timeline", "view()")) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = root.querySelectorAll(".hu-scroll-reveal-stagger > *");
    if (elements.length === 0) return;
    if (!this._scrollEntranceObserver) {
      this._scrollEntranceObserver = new IntersectionObserver(
        (entries) => {
          entries.forEach((e) => {
            if (e.isIntersecting) {
              (e.target as HTMLElement).classList.add("entered");
              this._scrollEntranceObserver?.unobserve(e.target);
            }
          });
        },
        { threshold: 0.1 },
      );
    }
    elements.forEach((el) => this._scrollEntranceObserver!.observe(el));
  }

  private _startDurationTimer(): void {
    if (this._durationTimer) return;
    this._sessionStartTs = Date.now();
    this._sessionDurationSec = 0;
    this._durationTimer = setInterval(() => {
      if (this._sessionStartTs) {
        this._sessionDurationSec = Math.floor((Date.now() - this._sessionStartTs) / 1000);
        this.requestUpdate();
      }
    }, 1000);
  }

  private _stopDurationTimer(): void {
    if (this._durationTimer) {
      clearInterval(this._durationTimer);
      this._durationTimer = null;
    }
    this._sessionStartTs = null;
    this._sessionDurationSec = 0;
  }

  private _formatDuration(sec: number): string {
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    return m > 0 ? `${m}:${String(s).padStart(2, "0")}` : `0:${String(s).padStart(2, "0")}`;
  }

  async #teardownVoiceGateway(gw: GatewayClientClass | null): Promise<void> {
    gw?.setOnBinaryChunk?.(null);
    if (this.#voiceStreaming && gw && typeof gw.voiceSessionStop === "function") {
      try {
        await gw.voiceSessionStop();
      } catch {
        /* ignore */
      }
    }
    this.#voiceStreaming = false;
    this.#activeSessionIsGL = false;
    this.#clearProcessingTimeout();
    this.#playback.interrupt();
    this._speaking = false;
    if (this.voiceStatus !== "idle") {
      this.voiceStatus = "idle";
    }
  }

  #armProcessingTimeout(): void {
    this.#clearProcessingTimeout();
    this.#processingTimer = setTimeout(() => {
      if (this.voiceStatus === "processing") {
        this.voiceStatus = "idle";
        this.requestUpdate();
      }
    }, 15_000);
  }

  #clearProcessingTimeout(): void {
    if (this.#processingTimer) {
      clearTimeout(this.#processingTimer);
      this.#processingTimer = null;
    }
  }

  async #onGatewayDisconnectedDuringVoice(): Promise<void> {
    this.#clearProcessingTimeout();
    const gw = this.gateway ?? this._boundGateway;
    gw?.setOnBinaryChunk?.(null);
    if (this._recorder.isRecording) {
      this._recorder.dispose();
    }
    const wasActive =
      this.voiceStatus === "listening" ||
      this.voiceStatus === "processing" ||
      this.#voiceStreaming ||
      this._speaking;
    this.#voiceStreaming = false;
    this.#activeSessionIsGL = false;
    this.#playback.interrupt();
    this._speaking = false;
    if (this.voiceStatus === "listening" || this.voiceStatus === "processing") {
      this.voiceStatus = "idle";
    }
    if (wasActive) {
      ScToast.show({
        message: "Connection lost — voice stopped. Reconnect when ready.",
        variant: "info",
      });
    }
    this.requestUpdate();
  }

  async #onGatewayReconnected(): Promise<void> {
    /* Stop any active voice session and reset to idle on reconnect.
       Don't auto-start — let the user choose when to resume. */
    const gw = this.gateway ?? this._boundGateway;
    gw?.setOnBinaryChunk?.(null);
    if (this._recorder.isRecording) {
      this._recorder.dispose();
    }
    if (this.#voiceStreaming && gw && typeof gw.voiceSessionStop === "function") {
      void gw.voiceSessionStop().catch(() => {});
    }
    this.#voiceStreaming = false;
    this.#activeSessionIsGL = false;
    this.#clearProcessingTimeout();
    this.#playback.interrupt();
    this._speaking = false;
    this._userVadSpeaking = false;
    this.voiceStatus = "idle";
    this.requestUpdate();
  }

  private _newSession(): void {
    void (async () => {
      if (this._recorder.isRecording) {
        this._recorder.dispose();
      }
      await this.#teardownVoiceGateway(this.gateway);
      this._messages = [];
      this.voiceStatus = "idle";
      this._stopDurationTimer();
      this._cacheMessages();
      this._incrementSessionCount();
      this.requestUpdate();
      ScToast.show({ message: "New session started", variant: "info" });
    })();
  }

  private _exportConversation(): void {
    if (this._messages.length === 0) {
      ScToast.show({ message: "No conversation to export", variant: "info" });
      return;
    }
    const lines = this._messages.map((m) => `**${m.role}:**\n${m.content}`);
    const md = lines.join("\n\n");
    const blob = new Blob([md], { type: "text/markdown" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `voice-conversation-${new Date().toISOString().slice(0, 10)}.md`;
    a.click();
    URL.revokeObjectURL(url);
    ScToast.show({ message: "Conversation exported", variant: "success" });
  }

  private get _chatItems(): ChatItem[] {
    return this._messages.map((m) => ({
      type: "message" as const,
      role: m.role,
      content: m.content,
      ts: m.ts,
    }));
  }

  private _findLastAssistantIdx(): number {
    for (let i = this._messages.length - 1; i >= 0; i--) {
      if (this._messages[i].role === "assistant") return i;
    }
    return -1;
  }

  /* Tool execution happens server-side in cp_voice_stream.c — the gateway
     dispatches tools against the agent's registered tool set and sends
     results back to Gemini. No client-side response needed. */

  private onGatewayEvent(e: Event): void {
    const ev = e as CustomEvent<{
      event: string;
      payload?: Record<string, unknown>;
    }>;
    const detail = ev.detail;
    if (!detail?.event) return;

    if (detail.event === "voice.transcript") {
      const text = (detail.payload?.text as string) ?? "";
      if (text) {
        const wasEmpty = this._messages.length === 0;
        this._messages = [...this._messages, { role: "user", content: text, ts: Date.now() }];
        this.transcript = text;
        if (wasEmpty) this._startDurationTimer();
      }
      this._cacheMessages();
      requestAnimationFrame(() => this.requestUpdate());
      return;
    }

    if (detail.event === "voice.assistant.transcript") {
      const text = (detail.payload?.text as string) ?? "";
      if (text) {
        const last = this._messages[this._messages.length - 1];
        if (last && last.role === "assistant") {
          last.content += text;
          this._messages = [...this._messages];
        } else {
          this._messages = [
            ...this._messages,
            { role: "assistant", content: text, ts: Date.now() },
          ];
        }
      }
      this._cacheMessages();
      requestAnimationFrame(() => this.requestUpdate());
      return;
    }

    if (detail.event === "voice.tool_call") {
      const name = (detail.payload?.name as string) ?? "unknown";
      const callId = (detail.payload?.call_id as string) ?? "";
      const args = (detail.payload?.args as string) ?? "{}";
      this.dispatchEvent(
        new CustomEvent("voice-tool-call", {
          detail: { name, callId, args },
          bubbles: true,
          composed: true,
        }),
      );
      return;
    }

    if (detail.event === "voice.audio.done") {
      this.#clearProcessingTimeout();
      if (this._speaking) {
        this.#playback.markEndOfStream();
      } else if (this.voiceStatus !== "listening") {
        this.voiceStatus = "idle";
        this.requestUpdate();
      }
      return;
    }

    if (detail.event === "voice.audio.interrupted") {
      this.#clearProcessingTimeout();
      this.#playback.interrupt();
      this._speaking = false;
      this.voiceStatus = "idle";
      this.requestUpdate();
      return;
    }

    if (detail.event === "voice.generation_complete") {
      this.#clearProcessingTimeout();
      if (this._speaking) {
        this.#playback.markEndOfStream();
      } else if (this.voiceStatus !== "listening") {
        this.voiceStatus = "idle";
        this.requestUpdate();
      }
      return;
    }

    if (detail.event === "voice.tool_cancelled") {
      const ids = (detail.payload?.ids as string[]) ?? [];
      if (ids.length > 0) {
        ScToast.show({
          message: `Tool call${ids.length > 1 ? "s" : ""} cancelled`,
          variant: "info",
        });
      }
      this.requestUpdate();
      return;
    }

    if (detail.event === "voice.reconnected") {
      ScToast.show({ message: "Voice session reconnected", variant: "info" });
      return;
    }

    if (detail.event === "voice.setup_complete") {
      this.#clearProcessingTimeout();
      ScToast.show({ message: "Voice session ready", variant: "info" });
      return;
    }

    if (detail.event === "voice.vad.speech_started") {
      this._userVadSpeaking = true;
      this.requestUpdate();
      return;
    }

    if (detail.event === "voice.vad.speech_stopped") {
      this._userVadSpeaking = false;
      this.requestUpdate();
      return;
    }

    if (detail.event === "voice.user.transcript") {
      const text = (detail.payload?.text as string) ?? "";
      if (text) {
        const wasEmpty = this._messages.length === 0;
        this._messages = [...this._messages, { role: "user", content: text, ts: Date.now() }];
        if (wasEmpty) this._startDurationTimer();
        this._cacheMessages();
      }
      this.requestUpdate();
      return;
    }

    if (detail.event === "voice.session_resumption") {
      return;
    }

    if (detail.event === "voice.goaway") {
      ScToast.show({ message: "Voice server reconnecting…", variant: "info" });
      return;
    }

    if (detail.event === "voice.error") {
      const msg = (detail.payload?.message as string) ?? "Voice session error";
      ScToast.show({ message: msg, variant: "error" });
      this.#clearProcessingTimeout();
      if (this._recorder.isRecording) {
        this._recorder.dispose();
      }
      const gw = this.gateway;
      gw?.setOnBinaryChunk?.(null);
      if (this.#voiceStreaming && gw && typeof gw.voiceSessionStop === "function") {
        void gw.voiceSessionStop().catch(() => {});
      }
      this.#voiceStreaming = false;
      this.#activeSessionIsGL = false;
      this.#playback.interrupt();
      this._speaking = false;
      this.voiceStatus = "idle";
      this.requestUpdate();
      return;
    }

    if (detail.event !== "chat") return;
    const payload = detail.payload ?? {};
    const sessionKey =
      (payload.session_key as string) ?? (payload.sessionKey as string) ?? (payload.id as string);
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
    requestAnimationFrame(() => this.requestUpdate());
  }

  private async toggleMic(): Promise<void> {
    if (this.voiceStatus === "listening") {
      await this._stopRecording();
    } else {
      await this._startRecording();
    }
  }

  private async _startRecording(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;

    if (this._speaking || this.voiceStatus === "processing") {
      try {
        if (typeof gw.voiceSessionInterrupt === "function") {
          await gw.voiceSessionInterrupt();
        }
      } catch {
        /* ignore */
      }
      this.#playback.interrupt();
      this._speaking = false;
    }

    const canStream =
      typeof gw.voiceSessionStart === "function" &&
      typeof gw.sendBinary === "function" &&
      typeof gw.setOnBinaryChunk === "function";

    if (canStream) {
      try {
        const startParams: Record<string, unknown> = {
          sessionKey: SESSION_KEY_VOICE,
        };
        if (this._voiceMode !== "standard") {
          startParams.mode = this._voiceMode;
        }
        if (this._stsApiKey.trim()) {
          startParams.apiKey = this._stsApiKey.trim();
        }
        const result = (await gw.voiceSessionStart(startParams)) as Record<string, unknown>;
        const isGeminiLive = result?.mode === "gemini_live";
        const isProviderDuplex =
          isGeminiLive || result?.mode === "openai_realtime" || result?.mode === "realtime";
        this.#activeSessionIsGL = isProviderDuplex;

        gw.setOnBinaryChunk((ab) => {
          if (ab.byteLength === 0 || ab.byteLength % 4 !== 0) return;
          const f32 = new Float32Array(ab);
          if (f32.length > 0) {
            this._speaking = true;
            void this.#playback.pushChunk(f32).catch(() => {
              /* AudioContext init failed or was closed */
            });
          }
          this.requestUpdate();
        });
        this.#silence.reset();

        if (isProviderDuplex) {
          /* Provider duplex (Gemini Live / OpenAI RT): stream raw PCM16 */
          const inputRate =
            (result?.input_sample_rate as number) ??
            (result?.sample_rate as number) ??
            (isGeminiLive ? 16000 : 24000);
          await this._recorder.startRawPcmStreaming(
            (data) => {
              try {
                gw.sendBinary(data);
              } catch {
                /* socket closed */
              }
            },
            {
              sampleRate: inputRate,
              onLevel: (rms) => {
                this._audioLevel = rms;
                this.#silence.onLevel(rms);
              },
            },
          );
        } else {
          /* Standard: stream encoded WebM/Opus to Cartesia pipeline */
          await this._recorder.startStreaming(
            (data) => {
              try {
                gw.sendBinary(data);
              } catch {
                /* socket closed */
              }
            },
            {
              onLevel: (rms) => {
                this._audioLevel = rms;
                this.#silence.onLevel(rms);
              },
            },
          );
        }
        this.#voiceStreaming = true;
        this.voiceStatus = "listening";
        return;
      } catch (err) {
        const raw = err instanceof Error ? err.message : String(err);
        let detail = raw;
        const low = raw.toLowerCase();
        if (low.includes("cartesia") || low.includes("api_key") || low.includes("api key")) {
          detail = "Voice session failed: check Cartesia API key in gateway config.";
        } else if (low.includes("timeout")) {
          detail = "Voice session timed out. Check network and gateway.";
        } else if (low.includes("not connected")) {
          detail = "Reconnect to the gateway, then try again.";
        } else if (low.includes("invalid") || low.includes("argument")) {
          detail = "Voice streaming unavailable (gateway or provider configuration).";
        } else if (low.includes("gemini") || low.includes("google")) {
          detail = "Gemini Live session failed: check Google API key in config.";
        }
        ScToast.show({ message: detail, variant: "error" });
        gw.setOnBinaryChunk(null);
        this.#voiceStreaming = false;
        if (typeof gw.voiceSessionStop === "function") {
          void gw.voiceSessionStop().catch(() => {});
        }
      }
    }

    try {
      await this._recorder.start();
      this.voiceStatus = "listening";
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Microphone access denied";
      ScToast.show({ message: msg, variant: "error" });
      this.voiceStatus = "idle";
    }
  }

  private async _stopRecording(): Promise<void> {
    if (this.voiceStatus !== "listening") return;
    this._audioLevel = 0;
    const gw = this.gateway;
    if (!gw) {
      this._recorder.dispose();
      this.voiceStatus = "idle";
      return;
    }

    if (this.#voiceStreaming) {
      this.voiceStatus = "processing";
      this.#armProcessingTimeout();
      try {
        await this._recorder.stopStreaming();
        if (this.#activeSessionIsGL) {
          await gw.voiceAudioEnd({ sessionKey: SESSION_KEY_VOICE });
        } else {
          const mime = this._recorder.streamMimeType || "audio/webm";
          await gw.voiceAudioEnd({
            mime_type: mime,
            sessionKey: SESSION_KEY_VOICE,
          });
        }
      } catch (err) {
        const msg = err instanceof Error ? err.message : "Voice stream failed";
        ScToast.show({ message: msg, variant: "error" });
        this.voiceStatus = "idle";
      }
      return;
    }

    this.voiceStatus = "processing";
    try {
      const { blob, mimeType } = await this._recorder.stop();
      const audio = await blobToBase64(blob);
      const result = await gw.request<{ text?: string }>("voice.transcribe", {
        audio,
        mime_type: mimeType,
      });
      if (result.text) {
        this.transcript = result.text;
        this.requestUpdate();
      }
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Transcription failed";
      ScToast.show({ message: msg, variant: "error" });
    }
    this.voiceStatus = "idle";
  }

  private async send(): Promise<void> {
    const text = this.transcript.trim();
    const gw = this.gateway;
    if (!text || !gw) return;
    this.error = "";
    const wasEmpty = this._messages.length === 0;
    this._messages = [...this._messages, { role: "user", content: text, ts: Date.now() }];
    if (wasEmpty) this._startDurationTimer();
    this.transcript = "";
    this.voiceStatus = "processing";
    this._cacheMessages();
    this.updateComplete.then(() => this._scrollConversation());
    try {
      await gw.request<{ status?: string; sessionKey?: string }>("chat.send", {
        message: text,
        sessionKey: SESSION_KEY_VOICE,
      });
    } catch (err) {
      this.voiceStatus = "idle";
      const msg = err instanceof Error ? err.message : "Failed to send message";
      this.error = msg;
      this._lastFailedMessage = text;
      ScToast.show({ message: msg, variant: "error" });
    }
  }

  private _retrySend(): void {
    this.error = "";
    const msg = this._lastFailedMessage;
    this._lastFailedMessage = "";
    if (!msg) return;
    this._messages = this._messages.slice(0, -1);
    this.transcript = msg;
    this._cacheMessages();
    this.updateComplete.then(() => this.send());
  }

  private _toggleSessionsPanel(): void {
    this._sessionsPanelOpen = !this._sessionsPanelOpen;
    try {
      localStorage.setItem("hu-voice-sessions-open", String(this._sessionsPanelOpen));
    } catch {
      /* ignore */
    }
  }

  private handleKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this.send();
    }
  }

  private _renderSetupBanner() {
    const providers: { id: string; label: string; help: string; url: string }[] = [
      {
        id: "gemini_live",
        label: "Gemini Live (Google)",
        help: "Get a key at",
        url: "https://aistudio.google.com/apikey",
      },
      {
        id: "openai_realtime",
        label: "OpenAI Realtime",
        help: "Get a key at",
        url: "https://platform.openai.com/api-keys",
      },
      {
        id: "standard",
        label: "Standard (Cartesia TTS)",
        help: "Get a key at",
        url: "https://play.cartesia.ai",
      },
    ];
    const selected = providers.find((p) => p.id === this._setupMode) ?? providers[0];
    return html`
      <div class="setup-banner" role="region" aria-label="Voice setup">
        <div
          style="display:flex;align-items:baseline;justify-content:space-between;gap:var(--hu-space-sm);margin-bottom:var(--hu-space-xs)"
        >
          <h3 style="margin:0;font:var(--hu-font-heading-sm)">Voice Setup</h3>
          <span style="font-size:var(--hu-text-xs);color:var(--hu-text-faint)"
            >Choose a provider and enter your API key</span
          >
        </div>
        <fieldset
          style="border:none;padding:0;margin:0 0 var(--hu-space-sm) 0;display:flex;flex-direction:row;gap:var(--hu-space-xs);flex-wrap:wrap"
          role="radiogroup"
          aria-label="Voice provider"
        >
          ${providers.map(
            (p) => html`
              <label class="provider-option" ?data-selected=${this._setupMode === p.id}>
                <input
                  type="radio"
                  name="voice-provider"
                  .value=${p.id}
                  .checked=${this._setupMode === p.id}
                  @change=${() => {
                    this._setupMode = p.id;
                    this._setupError = "";
                    this._setupSuccess = false;
                  }}
                />
                ${p.label}
              </label>
            `,
          )}
        </fieldset>
        <div style="display:flex;gap:var(--hu-space-xs);align-items:center">
          <input
            type="password"
            placeholder="Paste API key"
            aria-label="API key"
            .value=${this._setupKey}
            @input=${(e: InputEvent) => {
              this._setupKey = (e.target as HTMLInputElement).value;
              this._setupError = "";
            }}
            style="flex:1;padding:0.4375rem var(--hu-space-sm);border-radius:var(--hu-radius-md);border:1px solid var(--hu-border-subtle);background:var(--hu-bg-surface);color:var(--hu-text);font-size:var(--hu-text-sm);font-family:var(--hu-font);outline:none;transition:border-color var(--hu-duration-fast) var(--hu-ease-out)"
          />
          <button
            aria-label="Test connection"
            ?disabled=${this._setupValidating || !this._setupKey.trim()}
            @click=${() => void this._testVoiceConnection()}
            style="padding:0.4375rem var(--hu-space-md);border-radius:var(--hu-radius-md);border:none;background:var(--hu-accent);color:var(--hu-on-accent);font-size:var(--hu-text-sm);font-family:var(--hu-font);cursor:pointer;font-weight:var(--hu-weight-medium)"
          >
            ${this._setupValidating ? "Testing\u2026" : "Test"}
          </button>
          <a
            href=${selected.url}
            target="_blank"
            rel="noopener noreferrer"
            style="display:inline-flex;align-items:center;min-height:1.25rem;padding:var(--hu-space-2xs) 0;font-size:var(--hu-text-xs);color:var(--hu-text-faint);white-space:nowrap;text-decoration:underline;text-underline-offset:2px"
            >Get key &rarr;</a
          >
        </div>
        ${this._setupError
          ? html`<p
              style="margin:0 0 var(--hu-space-xs) 0;color:var(--hu-error);font:var(--hu-font-body-xs)"
            >
              ${this._setupError}
            </p>`
          : nothing}
        ${this._setupSuccess
          ? html`<p
              style="margin:0 0 var(--hu-space-xs) 0;color:var(--hu-success);font:var(--hu-font-body-xs)"
            >
              Connected successfully. Ready to talk.
            </p>`
          : nothing}
      </div>
    `;
  }

  private async _testVoiceConnection() {
    this._setupValidating = true;
    this._setupError = "";
    this._setupSuccess = false;
    try {
      const gw = this.gateway ?? this._boundGateway;
      if (!gw || typeof (gw as unknown as Record<string, unknown>).voiceValidate !== "function") {
        this._setupError = "Gateway not connected";
        return;
      }
      const result = await (
        gw as unknown as {
          voiceValidate: (p: {
            mode: string;
            apiKey: string;
          }) => Promise<{ ok: boolean; error?: string }>;
        }
      ).voiceValidate({
        mode: this._setupMode,
        apiKey: this._setupKey.trim(),
      });
      if (result.ok) {
        this._setupSuccess = true;
        try {
          localStorage.setItem("hu-voice-api-key", this._setupKey.trim());
          localStorage.setItem("hu-voice-mode", this._setupMode);
          if (this._setupMode === "gemini_live") {
            localStorage.setItem("hu-gemini-live", "true");
            this._geminiLiveMode = true;
          } else {
            localStorage.removeItem("hu-gemini-live");
            this._geminiLiveMode = false;
          }
        } catch {
          /* localStorage may be blocked */
        }
        setTimeout(() => {
          this._showSetup = false;
        }, 1200);
      } else {
        this._setupError = result.error ?? "Connection failed";
      }
    } catch (e) {
      this._setupError = e instanceof Error ? e.message : "Connection failed";
    } finally {
      this._setupValidating = false;
    }
  }

  override render() {
    if (this._loading) return this._renderSkeleton();
    return html`
      <div class="main-wrap">
        ${this._renderSessionsPanel()}
        <div class="container hu-mesh-gradient">
          ${this._renderStatusBar()} ${this._renderErrorBanner()}
          ${this._showSetup ? this._renderSetupBanner() : nothing}
          ${this._showClonePanel
            ? html`<hu-voice-clone .gateway=${this.gateway ?? this._boundGateway}></hu-voice-clone>`
            : nothing}
          ${this._showCredPanel ? this._renderCredPanel() : nothing}
          <hu-voice-conversation
            .items=${this._chatItems}
            .isWaiting=${this.voiceStatus === "processing"}
          ></hu-voice-conversation>
          ${this._renderControls()}
        </div>
      </div>
    `;
  }

  private _renderSessionsPanel() {
    const sessionCount = this._sessionCount;
    return html`
      <div
        class="sessions-panel ${this._sessionsPanelOpen ? "open" : ""}"
        role="navigation"
        aria-label="Voice sessions"
      >
        <div class="sessions-panel-inner">
          <div class="sessions-panel-header">Voice Sessions</div>

          <div class="sessions-panel-section">
            <h4 id="voice-sessions-listbox-label">Sessions</h4>
            <div role="listbox" aria-labelledby="voice-sessions-listbox-label">
              <button
                type="button"
                class="session-item active"
                role="option"
                aria-selected="true"
                aria-label="Current session"
              >
                <span class="session-item-icon">${icons.mic}</span>
                <span class="session-item-label"
                  >Session ${sessionCount}
                  ${this._messages.length > 0
                    ? html` · ${this._messages.length} msg${this._messages.length !== 1 ? "s" : ""}`
                    : nothing}</span
                >
              </button>
            </div>
          </div>

          <div class="sessions-panel-section">
            <h4>Voice Clones</h4>
            ${this._showClonePanel
              ? html`<button
                  type="button"
                  class="session-item"
                  @click=${() => {
                    this._showClonePanel = true;
                    if (window.innerWidth < 768) this._sessionsPanelOpen = false;
                  }}
                  aria-label="Manage voice clones"
                >
                  <span class="session-item-icon">${icons.radio}</span>
                  <span class="session-item-label">Manage clones</span>
                </button>`
              : html`<button
                  type="button"
                  class="session-item"
                  @click=${() => {
                    this._showClonePanel = true;
                    if (window.innerWidth < 768) this._sessionsPanelOpen = false;
                  }}
                  aria-label="Open voice clone panel"
                >
                  <span class="session-item-icon">${icons.radio}</span>
                  <span class="session-item-label">Clone voice</span>
                </button>`}
          </div>

          <div class="sessions-panel-section">
            <h4>Transcripts</h4>
            ${this._messages.length > 0
              ? html`<button
                  type="button"
                  class="session-item"
                  @click=${() => {
                    this._exportConversation();
                    if (window.innerWidth < 768) this._sessionsPanelOpen = false;
                  }}
                  aria-label="Export current transcript"
                >
                  <span class="session-item-icon">${icons["clock-counter-clockwise"]}</span>
                  <span class="session-item-label">Export transcript</span>
                </button>`
              : html`<div class="sessions-panel-empty">No transcripts yet</div>`}
          </div>
        </div>
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <div class="container hu-mesh-gradient">
        <hu-skeleton variant="card" class="skeleton-bar"></hu-skeleton>
        <hu-skeleton variant="card" style="flex:1"></hu-skeleton>
        <div class="skeleton-controls">
          <hu-skeleton variant="circle" width="72px" height="72px"></hu-skeleton>
          <hu-skeleton variant="card" height="60px" style="width:100%"></hu-skeleton>
        </div>
      </div>
    `;
  }

  private _renderStatusBar() {
    const connLabel =
      this._connectionStatus === "connected"
        ? "Connected"
        : this._connectionStatus === "connecting"
          ? "Reconnecting\u2026"
          : "Disconnected";

    const durationLabel =
      this._sessionDurationSec > 0 ? this._formatDuration(this._sessionDurationSec) : "";

    return html`
      <div class="status-bar" role="region" aria-label="Voice status">
        <div class="status-left">
          <button
            type="button"
            class="sessions-toggle"
            @click=${this._toggleSessionsPanel}
            aria-label=${this._sessionsPanelOpen ? "Close sessions panel" : "Open sessions panel"}
          >
            ${icons["sidebar-toggle"]}
          </button>
          <hu-status-dot
            .status=${this._connectionStatus === "connected" ? "healthy" : "offline"}
          ></hu-status-dot>
          <span class="status-title">Voice</span>
          <span class="status-meta">${connLabel}</span>
          ${durationLabel ? html`<span class="status-meta">· ${durationLabel}</span>` : nothing}
          ${this._messages.length > 0
            ? html`<span class="status-meta"
                >· ${this._messages.length} message${this._messages.length !== 1 ? "s" : ""}</span
              >`
            : nothing}
        </div>
        <div class="status-right">
          <hu-button
            variant="ghost"
            size="sm"
            ?disabled=${this._messages.length === 0}
            @click=${this._exportConversation}
            aria-label="Export conversation"
          >
            Export
          </hu-button>
          <hu-button
            variant=${this._voiceMode !== "standard" ? "tonal" : "ghost"}
            size="sm"
            ?disabled=${this.voiceStatus !== "idle"}
            @click=${() => {
              const modes: Array<"standard" | "gemini_live" | "openai_realtime"> = [
                "standard",
                "gemini_live",
                "openai_realtime",
              ];
              const idx = modes.indexOf(this._voiceMode);
              this._voiceMode = modes[(idx + 1) % modes.length];
              this._geminiLiveMode = this._voiceMode === "gemini_live";
              try {
                localStorage.setItem("hu-voice-mode", this._voiceMode);
              } catch {
                /* ignore */
              }
            }}
            aria-label="Cycle voice mode"
            title="Voice mode: standard (Cartesia), gemini_live, openai_realtime"
          >
            ${this._voiceMode === "gemini_live"
              ? "Gemini Live"
              : this._voiceMode === "openai_realtime"
                ? "OpenAI Realtime"
                : "Standard Voice"}
          </hu-button>
          <hu-button
            variant=${this._showCredPanel ? "tonal" : "ghost"}
            size="sm"
            @click=${() => {
              this._showCredPanel = !this._showCredPanel;
            }}
            aria-label="API credentials"
            title="Enter API keys for voice providers"
          >
            API Keys
          </hu-button>
          <hu-button
            variant="ghost"
            size="sm"
            @click=${() => {
              this._showClonePanel = !this._showClonePanel;
            }}
            aria-label="Clone voice"
          >
            Clone Voice
          </hu-button>
          <hu-button
            variant="ghost"
            size="sm"
            @click=${this._newSession}
            aria-label="Start new session"
          >
            New Session
          </hu-button>
        </div>
      </div>
    `;
  }

  private _renderErrorBanner() {
    if (!this.error) return nothing;
    return html`
      <div class="error-banner" role="alert">
        <span>${this.error}</span>
        <hu-button variant="ghost" size="sm" @click=${this._retrySend} aria-label="Retry">
          Retry
        </hu-button>
      </div>
    `;
  }

  private _renderCredPanel() {
    return html`
      <div
        style="padding: var(--hu-space-md); background: var(--hu-surface-container); border-bottom: 1px solid var(--hu-border-subtle); display: flex; flex-direction: column; gap: var(--hu-space-sm); flex-shrink: 0;"
      >
        <div
          style="font-size: var(--hu-text-sm); font-weight: var(--hu-weight-medium); color: var(--hu-text);"
        >
          Voice Provider Credentials
        </div>
        <div style="font-size: var(--hu-text-xs); color: var(--hu-text-secondary);">
          ${this._voiceMode === "gemini_live"
            ? "Enter your Google AI / Vertex AI API key for Gemini Live."
            : this._voiceMode === "openai_realtime"
              ? "Enter your OpenAI API key for Realtime voice."
              : "Enter your Cartesia API key (or leave blank to use server config)."}
        </div>
        <div style="display: flex; gap: var(--hu-space-sm); align-items: flex-end;">
          <hu-input
            type="password"
            placeholder="API key"
            .value=${this._stsApiKey}
            @hu-input=${(e: CustomEvent<{ value: string }>) => {
              this._stsApiKey = e.detail.value;
            }}
            style="flex: 1;"
            .accessibleLabel=${"Voice provider API key"}
          ></hu-input>
          <hu-button
            variant="ghost"
            size="sm"
            @click=${() => {
              this._stsApiKey = "";
              try {
                localStorage.removeItem("hu-sts-api-key");
              } catch {
                /* ignore */
              }
              ScToast.show({ message: "API key cleared", variant: "info" });
            }}
          >
            Clear
          </hu-button>
          <hu-button
            variant="tonal"
            size="sm"
            @click=${() => {
              try {
                localStorage.setItem("hu-sts-api-key", this._stsApiKey);
              } catch {
                /* ignore */
              }
              this._showCredPanel = false;
              ScToast.show({ message: "API key saved for this session", variant: "success" });
            }}
          >
            Save
          </hu-button>
        </div>
      </div>
    `;
  }

  private _renderControls() {
    const micDisabled = !this._recorder.isSupported || this._connectionStatus === "disconnected";
    return html`
      <div class="controls-zone hu-scroll-reveal-stagger">
        <hu-voice-orb
          .state=${this._orbVisualState}
          .audioLevel=${this._userVadSpeaking ? Math.max(this._audioLevel, 0.4) : this._audioLevel}
          ?disabled=${micDisabled}
          @hu-voice-mic-toggle=${this.toggleMic}
        ></hu-voice-orb>
        <div class="input-row">
          <hu-textarea
            placeholder="Speak or type a message…"
            .value=${this.transcript}
            ?disabled=${this._connectionStatus === "disconnected"}
            @hu-input=${(e: CustomEvent<{ value: string }>) => {
              this.transcript = e.detail.value;
            }}
            @keydown=${this.handleKeyDown}
            resize="none"
            rows="2"
            .accessibleLabel=${"Voice message input"}
          ></hu-textarea>
          <hu-button
            variant="primary"
            ?disabled=${!this.transcript.trim() ||
            this.voiceStatus === "processing" ||
            this._connectionStatus === "disconnected"}
            @click=${() => this.send()}
            aria-label="Send voice message"
          >
            Send
          </hu-button>
        </div>
      </div>
    `;
  }
}
