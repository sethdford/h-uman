import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { ScToast } from "./hu-toast.js";
import { AudioRecorder, blobToBase64 } from "../audio-recorder.js";
import type { GatewayClient } from "../gateway.js";
import { icons } from "../icons.js";
import "./hu-button.js";

type ClonePhase = "idle" | "recording" | "uploading" | "done" | "error";

@customElement("hu-voice-clone")
export class HuVoiceClone extends LitElement {
  gateway: GatewayClient | null = null;

  @state() private _phase: ClonePhase = "idle";
  @state() private _voiceName = "";
  @state() private _persona = "";
  @state() private _voiceId = "";
  @state() private _recordingDuration = 0;
  @state() private _error = "";

  private _recorder = new AudioRecorder();
  private _timer: ReturnType<typeof setInterval> | null = null;

  static override styles = css`
    :host {
      display: block;
    }

    .clone-card {
      background: var(--hu-surface-container);
      border-radius: var(--hu-radius-lg);
      padding: var(--hu-space-xl);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-lg);
    }

    .clone-header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
    }

    .clone-header h3 {
      margin: 0;
      font: var(--hu-font-heading-sm);
      color: var(--hu-text);
    }

    .clone-header .badge {
      font: var(--hu-font-caption);
      color: var(--hu-accent-text);
      background: var(--hu-accent-subtle);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-full);
    }

    .clone-description {
      font: var(--hu-font-body);
      color: var(--hu-text-secondary);
      margin: 0;
      line-height: 1.5;
    }

    .form-row {
      display: flex;
      gap: var(--hu-space-md);
      align-items: end;
    }

    .form-field {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
      flex: 1;
    }

    .form-field label {
      font: var(--hu-font-caption);
      color: var(--hu-text-secondary);
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }

    .form-field input {
      font: var(--hu-font-body);
      color: var(--hu-text);
      background: var(--hu-surface-container-high);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-md);
      padding: var(--hu-space-sm) var(--hu-space-md);
      outline: none;
      transition: border-color var(--hu-duration-fast) var(--hu-ease-standard);
    }

    .form-field input:focus {
      border-color: var(--hu-accent);
    }

    .form-field input::placeholder {
      color: var(--hu-text-tertiary);
    }

    .recording-indicator {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-md) var(--hu-space-lg);
      background: color-mix(in srgb, var(--hu-error) 10%, transparent);
      border-radius: var(--hu-radius-md);
      font: var(--hu-font-body);
      color: var(--hu-text);
    }

    .recording-dot {
      width: var(--hu-space-sm);
      height: var(--hu-space-sm);
      background: var(--hu-error);
      border-radius: var(--hu-radius-full);
      animation: hu-recording-pulse var(--hu-duration-slow) var(--hu-ease-in-out, ease-in-out)
        infinite;
    }

    @keyframes hu-recording-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.3;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .recording-dot {
        animation: none;
      }
    }

    .recording-time {
      font-variant-numeric: tabular-nums;
      font: var(--hu-font-heading-sm);
    }

    .result-card {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
      padding: var(--hu-space-md) var(--hu-space-lg);
      background: color-mix(in srgb, var(--hu-accent) 10%, transparent);
      border-radius: var(--hu-radius-md);
    }

    .result-check {
      color: var(--hu-accent);
      width: 1.25rem;
      height: 1.25rem;
      flex-shrink: 0;
    }

    .result-details {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .result-title {
      font: var(--hu-font-body-strong);
      color: var(--hu-text);
    }

    .result-id {
      font: var(--hu-font-caption);
      color: var(--hu-text-secondary);
      font-family: var(--hu-font-mono);
    }

    .error-msg {
      color: var(--hu-error);
      font: var(--hu-font-body);
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: color-mix(in srgb, var(--hu-error) 8%, transparent);
      border-radius: var(--hu-radius-md);
    }

    .actions {
      display: flex;
      gap: var(--hu-space-sm);
      flex-wrap: wrap;
    }
  `;

  override render() {
    return html`
      <div class="clone-card">
        <div class="clone-header">
          <h3>Voice Clone</h3>
          <span class="badge">Cartesia</span>
        </div>
        <p class="clone-description">
          Record 5–10 seconds of clear speech to create a voice clone. The cloned voice will be used
          for text-to-speech in your conversations.
        </p>

        ${
          this._phase !== "done"
            ? html`
                <div class="form-row">
                  <div class="form-field">
                    <label>Voice Name</label>
                    <input
                      type="text"
                      placeholder="My Voice"
                      .value=${this._voiceName}
                      @input=${(e: InputEvent) => {
                        this._voiceName = (e.target as HTMLInputElement).value;
                      }}
                    />
                  </div>
                  <div class="form-field">
                    <label>Persona (optional)</label>
                    <input
                      type="text"
                      placeholder="Leave blank to skip"
                      .value=${this._persona}
                      @input=${(e: InputEvent) => {
                        this._persona = (e.target as HTMLInputElement).value;
                      }}
                    />
                  </div>
                </div>
              `
            : nothing
        }
        ${
          this._phase === "recording"
            ? html`
                <div class="recording-indicator">
                  <div class="recording-dot"></div>
                  <span>Recording</span>
                  <span class="recording-time"
                    >${this._formatDuration(this._recordingDuration)}</span
                  >
                </div>
              `
            : nothing
        }
        ${
          this._phase === "done"
            ? html`
                <div class="result-card">
                  <span class="result-check">${icons.check}</span>
                  <div class="result-details">
                    <span class="result-title">Voice cloned: ${this._voiceName || "My Voice"}</span>
                    <span class="result-id">${this._voiceId}</span>
                  </div>
                </div>
              `
            : nothing
        }
        ${
          this._phase === "error"
            ? html`<div class="error-msg">${this._error || "Clone failed"}</div>`
            : nothing
        }

        <div class="actions">
          ${
            this._phase === "idle" || this._phase === "error"
              ? html`
                  <hu-button
                    variant="primary"
                    @click=${this._startRecording}
                    ?disabled=${!this._recorder.isSupported}
                  >
                    Start Recording
                  </hu-button>
                `
              : nothing
          }
          ${
            this._phase === "recording"
              ? html`
                  <hu-button
                    variant="primary"
                    @click=${this._stopAndUpload}
                    ?disabled=${this._recordingDuration < 3}
                  >
                    Stop & Clone
                  </hu-button>
                  <hu-button variant="ghost" @click=${this._cancelRecording}>Cancel</hu-button>
                `
              : nothing
          }
          ${
            this._phase === "uploading"
              ? html` <hu-button variant="primary" disabled>Cloning…</hu-button> `
              : nothing
          }
          ${
            this._phase === "done"
              ? html` <hu-button variant="ghost" @click=${this._reset}>Clone Another</hu-button> `
              : nothing
          }
        </div>
      </div>
    `;
  }

  private _formatDuration(sec: number): string {
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    return `${m}:${String(s).padStart(2, "0")}`;
  }

  private async _startRecording(): Promise<void> {
    try {
      await this._recorder.start();
      this._phase = "recording";
      this._recordingDuration = 0;
      this._error = "";
      this._timer = setInterval(() => {
        this._recordingDuration++;
      }, 1000);
    } catch {
      this._error = "Microphone access denied";
      this._phase = "error";
    }
  }

  private async _stopAndUpload(): Promise<void> {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }

    try {
      const capture = await this._recorder.stop();
      if (!capture || !capture.blob || capture.blob.size === 0) {
        this._error = "No audio recorded";
        this._phase = "error";
        return;
      }

      this._phase = "uploading";
      const base64 = await blobToBase64(capture.blob);

      if (!this.gateway) {
        this._error = "Not connected to gateway";
        this._phase = "error";
        return;
      }

      const result = (await this.gateway.request("voice.clone", {
        audio: base64,
        mimeType: capture.mimeType || "audio/wav",
        name: this._voiceName || "My Voice",
        language: "en",
        ...(this._persona ? { persona: this._persona } : {}),
      })) as { voice_id?: string; name?: string };

      if (result?.voice_id) {
        this._voiceId = result.voice_id;
        this._phase = "done";
        ScToast.show({ message: "Voice cloned successfully!", variant: "success" });
      } else {
        this._error = "No voice ID returned";
        this._phase = "error";
      }
    } catch (err) {
      this._error = err instanceof Error ? err.message : "Clone failed";
      this._phase = "error";
    }
  }

  private _cancelRecording(): void {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
    this._recorder.dispose();
    this._phase = "idle";
    this._recordingDuration = 0;
  }

  private _reset(): void {
    this._phase = "idle";
    this._voiceId = "";
    this._voiceName = "";
    this._persona = "";
    this._recordingDuration = 0;
    this._error = "";
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
    if (this._recorder.isRecording) {
      this._recorder.dispose();
    }
  }
}
