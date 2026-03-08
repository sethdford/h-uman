import { LitElement, html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { AudioRecorder, blobToBase64 } from "../audio-recorder.js";

@customElement("sc-floating-mic")
export class ScFloatingMic extends LitElement {
  static override styles = css`
    :host {
      position: fixed;
      right: var(--sc-space-lg);
      bottom: var(--sc-space-lg);
      z-index: 9999;
    }
    .btn {
      width: 3rem;
      height: 3rem;
      border-radius: 50%;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 0;
      transition: background var(--sc-duration-normal);
    }
    .btn:hover {
      background: var(--sc-accent-hover);
    }
    .btn:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }
    .btn.listening {
      background: var(--sc-error);
      animation: sc-pulse-red var(--sc-duration-slow) ease-in-out infinite;
    }
    .btn.transcribing {
      background: var(--sc-accent-secondary);
      opacity: 0.8;
      cursor: wait;
    }
    @keyframes sc-pulse-red {
      0%,
      100% {
        box-shadow: 0 0 0 0 color-mix(in srgb, var(--sc-error) 50%, transparent);
      }
      50% {
        box-shadow: 0 0 0 0.75rem color-mix(in srgb, var(--sc-error) 0%, transparent);
      }
    }
    .btn svg {
      width: var(--sc-icon-lg);
      height: var(--sc-icon-lg);
    }
    @media (prefers-reduced-motion: reduce) {
      .btn.listening {
        animation: none;
        box-shadow: 0 0 0 var(--sc-space-xs) color-mix(in srgb, var(--sc-error) 30%, transparent);
      }
    }
    .overlay {
      position: absolute;
      bottom: calc(100% + var(--sc-space-sm));
      right: 0;
      min-width: 12.5rem;
      max-width: 18.75rem;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font-mono);
      color: var(--sc-text);
      max-height: 7.5rem;
      overflow-y: auto;
    }
  `;

  @state() private isListening = false;
  @state() private isTranscribing = false;
  @state() private overlayText = "";
  private _recorder = new AudioRecorder();

  override connectedCallback(): void {
    super.connectedCallback();
    this._setupKeyboardShortcut();
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this._removeKeyboardShortcut();
    this._recorder.dispose();
  }

  private _handleKeyDown = (e: KeyboardEvent): void => {
    if ((e.metaKey || e.ctrlKey) && e.shiftKey && e.key === "m") {
      e.preventDefault();
      this.toggleRecording();
    }
  };

  private _setupKeyboardShortcut(): void {
    window.addEventListener("keydown", this._handleKeyDown);
  }

  private _removeKeyboardShortcut(): void {
    window.removeEventListener("keydown", this._handleKeyDown);
  }

  private toggleRecording(): void {
    if (!this._recorder.isSupported || this.isTranscribing) return;
    if (this.isListening) {
      this._stopAndTranscribe();
    } else {
      this._startRecording();
    }
  }

  private async _startRecording(): Promise<void> {
    try {
      await this._recorder.start();
      this.isListening = true;
      this.overlayText = "Recording\u2026";
    } catch {
      this.isListening = false;
      this.overlayText = "";
    }
  }

  private async _stopAndTranscribe(): Promise<void> {
    this.isListening = false;
    this.isTranscribing = true;
    this.overlayText = "Transcribing\u2026";

    try {
      const { blob, mimeType } = await this._recorder.stop();
      const audio = await blobToBase64(blob);

      const detail = { audio, mimeType };
      const event = new CustomEvent<{ audio: string; mimeType: string }>("sc-voice-transcribe", {
        bubbles: true,
        composed: true,
        detail,
      });

      const responded = new Promise<string>((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Transcription timeout")), 30_000);
        const handler = (e: Event) => {
          clearTimeout(timeout);
          window.removeEventListener("sc-voice-transcript-result", handler);
          const text = (e as CustomEvent<{ text: string }>).detail.text;
          resolve(text);
        };
        window.addEventListener("sc-voice-transcript-result", handler);
      });

      this.dispatchEvent(event);
      const text = await responded;

      if (text.trim()) {
        this._injectTranscript(text);
      }
    } catch {
      /* transcription failed silently */
    }

    this.isTranscribing = false;
    this.overlayText = "";
  }

  private _findInput(
    root: Document | ShadowRoot,
    depth: number,
  ): HTMLTextAreaElement | HTMLInputElement | null {
    if (depth > 4) return null;
    const direct = root.querySelector("textarea, input") as
      | HTMLTextAreaElement
      | HTMLInputElement
      | null;
    if (direct) return direct;
    const hosts = root.querySelectorAll("*");
    for (const el of hosts) {
      if (el.shadowRoot) {
        const found = this._findInput(el.shadowRoot, depth + 1);
        if (found) return found;
      }
    }
    return null;
  }

  private _injectTranscript(text: string): void {
    if (!text.trim()) return;

    let target: HTMLTextAreaElement | HTMLInputElement | null = null;

    const deepActive = (root: Document | ShadowRoot): Element | null => {
      const a = root.activeElement;
      if (!a) return null;
      if (a.shadowRoot) return deepActive(a.shadowRoot) ?? a;
      return a;
    };
    const active = deepActive(document) as HTMLElement | null;
    if (active && (active.tagName === "TEXTAREA" || active.tagName === "INPUT")) {
      target = active as HTMLTextAreaElement | HTMLInputElement;
    }

    if (!target) {
      const appRoot = document.querySelector("sc-app")?.shadowRoot;
      if (appRoot) target = this._findInput(appRoot, 0);
    }
    if (!target) return;

    const start = target.selectionStart ?? target.value.length;
    const end = target.selectionEnd ?? target.value.length;
    const before = target.value.slice(0, start);
    const after = target.value.slice(end);
    const separator = before && !before.endsWith(" ") && !text.startsWith(" ") ? " " : "";
    const newValue = before + separator + text.trim() + after;
    target.value = newValue;
    const pos = start + separator.length + text.trim().length;
    target.setSelectionRange(pos, pos);
    target.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
  }

  override render() {
    const btnClass = this.isListening ? "listening" : this.isTranscribing ? "transcribing" : "";
    return html`
      <div>
        ${this.isListening || this.isTranscribing
          ? html`<div class="overlay">${this.overlayText}</div>`
          : ""}
        <button
          class="btn ${btnClass}"
          ?disabled=${!this._recorder.isSupported || this.isTranscribing}
          title=${this._recorder.isSupported
            ? "Start voice input (Cmd+Shift+M)"
            : "Audio recording not supported"}
          @click=${this.toggleRecording}
          aria-label="Toggle voice input"
        >
          ${icons.mic}
        </button>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-floating-mic": ScFloatingMic;
  }
}
