import { LitElement, html, css } from "lit";
import { customElement, state } from "lit/decorators.js";

@customElement("sc-floating-mic")
export class ScFloatingMic extends LitElement {
  static override styles = css`
    :host {
      position: fixed;
      right: 1.5rem;
      bottom: 1.5rem;
      z-index: 9999;
    }
    .btn {
      width: 48px;
      height: 48px;
      border-radius: 50%;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 0;
      transition: background 0.2s;
    }
    .btn:hover {
      background: var(--sc-accent-hover);
    }
    .btn:disabled {
      opacity: 0.6;
      cursor: not-allowed;
    }
    .btn.listening {
      background: var(--sc-error);
      animation: pulse-red 1s ease-in-out infinite;
    }
    @keyframes pulse-red {
      0%,
      100% {
        box-shadow: 0 0 0 0 rgba(248, 81, 73, 0.5);
      }
      50% {
        box-shadow: 0 0 0 12px rgba(248, 81, 73, 0);
      }
    }
    .btn svg {
      width: 24px;
      height: 24px;
    }
    .overlay {
      position: absolute;
      bottom: calc(100% + 0.5rem);
      right: 0;
      min-width: 200px;
      max-width: 300px;
      padding: 0.5rem 0.75rem;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      font-size: 0.8125rem;
      font-family: var(--sc-font-mono);
      color: var(--sc-text);
      max-height: 120px;
      overflow-y: auto;
    }
  `;

  @state() private isListening = false;
  @state() private liveTranscript = "";
  @state() private speechSupported = true;
  @state() private recognition: SpeechRecognition | null = null;

  private get hasSpeechRecognition(): boolean {
    const g =
      typeof window !== "undefined" ? (window as unknown as { SpeechRecognition?: unknown }) : null;
    const w = g ? (window as unknown as { webkitSpeechRecognition?: unknown }) : null;
    return !!(g?.SpeechRecognition ?? w?.webkitSpeechRecognition);
  }

  override connectedCallback(): void {
    super.connectedCallback();
    this.speechSupported = this.hasSpeechRecognition;
    this._setupKeyboardShortcut();
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this._removeKeyboardShortcut();
    this._stopRecognition();
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
    if (!this.speechSupported) return;
    if (this.isListening) {
      this._stopRecognition();
    } else {
      this._startRecognition();
    }
  }

  private _startRecognition(): void {
    if (!this.hasSpeechRecognition) return;
    const SR =
      (window as unknown as { SpeechRecognition?: new () => SpeechRecognition })
        .SpeechRecognition ??
      (
        window as unknown as {
          webkitSpeechRecognition?: new () => SpeechRecognition;
        }
      ).webkitSpeechRecognition;
    if (!SR) return;

    const rec = new SR();
    rec.continuous = true;
    rec.interimResults = true;
    rec.lang = "en-US";

    rec.onresult = (e: SpeechRecognitionEvent): void => {
      let interim = "";
      let final = "";
      for (let i = e.resultIndex; i < e.results.length; i++) {
        const res = e.results[i];
        const text = res[0].transcript;
        if (res.isFinal) {
          final += text;
        } else {
          interim += text;
        }
      }
      if (final) {
        this._injectTranscript(final);
        this.liveTranscript = "";
      } else {
        this.liveTranscript = interim;
      }
    };

    rec.onend = (): void => {
      this.isListening = false;
      this.liveTranscript = "";
    };

    rec.onerror = (): void => {
      this.isListening = false;
      this.liveTranscript = "";
    };

    try {
      rec.start();
      this.recognition = rec;
      this.isListening = true;
    } catch {
      this.isListening = false;
      this.recognition = null;
    }
  }

  private _stopRecognition(): void {
    if (this.recognition) {
      try {
        this.recognition.stop();
      } catch {
        /* ignore */
      }
      this.recognition = null;
    }
    this.isListening = false;
    this.liveTranscript = "";
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
    return html`
      <div>
        ${this.isListening && this.liveTranscript
          ? html`<div class="overlay">${this.liveTranscript}</div>`
          : ""}
        <button
          class="btn ${this.isListening ? "listening" : ""}"
          ?disabled=${!this.speechSupported}
          title=${this.speechSupported ? "Start voice input (Cmd+Shift+M)" : "Speech not supported"}
          @click=${this.toggleRecording}
          aria-label="Toggle voice input"
        >
          <svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2"
            stroke-linecap="round"
            stroke-linejoin="round"
          >
            <path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
            <path d="M19 10v2a7 7 0 0 1-14 0v-2" />
            <line x1="12" y1="19" x2="12" y2="23" />
            <line x1="8" y1="23" x2="16" y2="23" />
          </svg>
        </button>
      </div>
    `;
  }
}
