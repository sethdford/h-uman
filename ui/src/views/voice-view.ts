import { html, css, nothing } from "lit";
import { customElement, query, state } from "lit/decorators.js";
import type { GatewayClient } from "../gateway.js";
import { GatewayClient as GatewayClientClass } from "../gateway.js";
import type { GatewayStatus } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { SESSION_KEY_VOICE } from "../utils.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { ScToast } from "../components/hu-toast.js";
import { AudioRecorder, blobToBase64 } from "../audio-recorder.js";
import "../components/hu-button.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-status-dot.js";
import "../components/hu-voice-orb.js";
import "../components/hu-voice-conversation.js";

type VoiceStatus = "idle" | "listening" | "processing" | "unsupported";

interface VoiceMessage {
  role: "user" | "assistant";
  content: string;
  ts: number;
}

@customElement("hu-voice-view")
export class ScVoiceView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-voice;
      display: flex;
      flex-direction: column;
      height: 100%;
      max-height: calc(100vh - var(--hu-space-5xl));
    }

    .container {
      display: flex;
      flex-direction: column;
      flex: 1;
      height: 100%;
      max-width: 45rem;
      margin: 0 auto;
      position: relative;
      width: 100%;
      min-height: 0;
    }

    /* ── Status bar ─────────────────────────────────── */

    .status-bar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-xs) var(--hu-space-md);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      background: color-mix(in srgb, var(--hu-bg-surface) 60%, transparent);
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      border-bottom: 1px solid var(--hu-border-subtle);
      flex-shrink: 0;
    }

    .status-left,
    .status-right {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .status-title {
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
      font-size: var(--hu-text-sm);
    }

    .status-meta {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
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
      background: var(--hu-bg-surface);
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

    @media (max-width: 480px) /* --hu-breakpoint-sm */ {
      .input-row {
        flex-direction: column;
        align-items: stretch;
      }
      .input-row hu-button {
        min-height: 2.5rem;
      }
    }
  `;

  @state() private transcript = "";
  @state() private voiceStatus: VoiceStatus = "idle";
  @state() private error = "";
  @state() private _lastFailedMessage = "";
  @state() private _messages: VoiceMessage[] = [];
  @state() private _connectionStatus: GatewayStatus = "disconnected";
  @state() private _loading = true;
  @state() private _sessionStartTs: number | null = null;
  @state() private _sessionDurationSec = 0;
  private _durationTimer: ReturnType<typeof setInterval> | null = null;
  private _recorder = new AudioRecorder();

  private gatewayHandler = (e: Event): void => this.onGatewayEvent(e);
  private statusHandler = (e: Event): void => {
    this._connectionStatus = (e as CustomEvent<GatewayStatus>).detail;
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
      this._restoreFromCache();
      const gw = this.gateway;
      if (gw) {
        this._boundGateway = gw;
        this._connectionStatus = gw.status;
        gw.addEventListener(GatewayClientClass.EVENT_GATEWAY, this.gatewayHandler);
        gw.addEventListener(GatewayClientClass.EVENT_STATUS, this.statusHandler as EventListener);
      }
      this._loading = false;
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

  private _newSession(): void {
    this._messages = [];
    this._stopDurationTimer();
    this._cacheMessages();
    this._incrementSessionCount();
    this.requestUpdate();
    ScToast.show({ message: "New session started", variant: "info" });
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
    requestAnimationFrame(() => this.requestUpdate());
  }

  private async toggleMic(): Promise<void> {
    if (this.voiceStatus === "listening") {
      await this._stopAndTranscribe();
    } else {
      await this._startRecording();
    }
  }

  private async _startRecording(): Promise<void> {
    try {
      await this._recorder.start();
      this.voiceStatus = "listening";
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Microphone access denied";
      ScToast.show({ message: msg, variant: "error" });
      this.voiceStatus = "idle";
    }
  }

  private async _stopAndTranscribe(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this._recorder.dispose();
      this.voiceStatus = "idle";
      return;
    }

    this.voiceStatus = "processing";
    try {
      const { blob, mimeType } = await this._recorder.stop();
      const audio = await blobToBase64(blob);
      const result = await gw.request<{ text?: string }>("voice.transcribe", {
        audio,
        mimeType,
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

  private handleKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this.send();
    }
  }

  override render() {
    if (this._loading) return this._renderSkeleton();
    return html`
      <div class="container">
        ${this._renderStatusBar()} ${this._renderErrorBanner()}
        <hu-voice-conversation
          .items=${this._chatItems}
          .isWaiting=${this.voiceStatus === "processing"}
        ></hu-voice-conversation>
        ${this._renderControls()}
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <div class="container">
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

  private _renderControls() {
    const micDisabled = !this._recorder.isSupported || this._connectionStatus === "disconnected";
    return html`
      <div class="controls-zone">
        <hu-voice-orb
          .state=${this.voiceStatus}
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
