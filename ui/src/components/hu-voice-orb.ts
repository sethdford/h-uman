import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

export type VoiceOrbState = "idle" | "listening" | "speaking" | "processing" | "unsupported";

@customElement("hu-voice-orb")
export class ScVoiceOrb extends LitElement {
  @property({ type: String }) state: VoiceOrbState = "idle";
  @property({ type: Number }) audioLevel = 0;
  @property({ type: Boolean }) disabled = false;

  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: var(--hu-space-lg) 0 var(--hu-space-md);
      position: relative;
      flex-shrink: 0;
    }

    .mic-orb-glow {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -60%);
      width: 15rem;
      height: 15rem;
      border-radius: 50%;
      background: radial-gradient(circle, var(--hu-accent-subtle) 0%, transparent 70%);
      opacity: 0.4;
      pointer-events: none;
      transition: opacity var(--hu-duration-normal) var(--hu-ease-out);
    }

    .mic-orb-glow.active {
      opacity: 0.8;
      animation: hu-glow-breathe 3s ease-in-out infinite; /* hu-lint-ok: ambient */
    }

    @keyframes hu-glow-breathe {
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
      width: var(--hu-space-5xl);
      height: var(--hu-space-5xl);
      border-radius: 50%;
      border: 2px solid var(--hu-border);
      background: var(--hu-bg-surface);
      background-image: var(--hu-surface-gradient);
      color: var(--hu-text-muted);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      position: relative;
      box-shadow: var(--hu-shadow-card);
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-normal) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .mic-btn svg {
      width: 2.5rem;
      height: 2.5rem;
      filter: drop-shadow(0 1px 1px color-mix(in srgb, var(--hu-text) 10%, transparent));
    }

    .mic-btn:hover:not(:disabled) {
      background: var(--hu-hover-overlay);
      border-color: var(--hu-accent);
      color: var(--hu-accent-text, var(--hu-accent));
      box-shadow: var(--hu-shadow-md);
      transform: translateY(-2px);
    }

    .mic-btn:active:not(:disabled) {
      transform: translateY(1px) scaleY(0.97) scaleX(1.01);
    }

    .mic-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: var(--hu-space-xs);
    }

    .mic-btn:disabled {
      opacity: var(--hu-opacity-disabled, 0.5);
      cursor: not-allowed;
    }

    .mic-btn.active {
      background: var(--hu-accent);
      background-image: var(--hu-button-gradient-primary);
      border-color: var(--hu-accent);
      color: var(--hu-on-accent);
      box-shadow: var(--hu-shadow-glow-accent);
      transform: translateY(-1px);
    }

    .mic-ring {
      position: absolute;
      top: 50%;
      left: 50%;
      width: var(--hu-space-5xl);
      height: var(--hu-space-5xl);
      border-radius: 50%;
      border: 2px solid var(--hu-accent);
      transform: translate(-50%, -50%) scale(1);
      opacity: 0;
      pointer-events: none;
    }

    .mic-ring.active {
      animation: hu-ring-expand 2s ease-out infinite; /* hu-lint-ok: ambient */
    }

    .mic-ring.ring-2.active {
      animation-delay: 0.6s;
    }

    .mic-ring.ring-3.active {
      animation-delay: 1.2s;
    }

    @keyframes hu-ring-expand {
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
      margin-top: var(--hu-space-lg);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      position: relative;
      z-index: 1;
    }

    .voice-status.listening,
    .voice-status.processing {
      color: var(--hu-accent-text, var(--hu-accent));
      font-weight: var(--hu-weight-medium);
    }

    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
      .mic-btn {
        width: 4.5rem;
        height: 4.5rem;
      }
      .mic-btn svg {
        width: 2rem;
        height: 2rem;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .mic-btn.active {
        animation: none !important;
      }
      .mic-btn.active {
        box-shadow: var(--hu-shadow-glow-accent);
      }
    }
  `;

  private _handleClick(): void {
    if (this.disabled) return;
    this.dispatchEvent(
      new CustomEvent("hu-voice-mic-toggle", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private get _statusText(): string {
    switch (this.state) {
      case "listening":
        return "Listening…";
      case "processing":
        return "Processing…";
      case "unsupported":
        return "Speech recognition not supported in this browser";
      default:
        return "Click the microphone to start speaking";
    }
  }

  override render() {
    const isActive = this.state === "listening";
    return html`
      <div class="mic-orb-glow ${isActive ? "active" : ""}"></div>
      <div class="mic-btn-wrap">
        <div class="mic-ring ${isActive ? "active" : ""}"></div>
        <div class="mic-ring ring-2 ${isActive ? "active" : ""}"></div>
        <div class="mic-ring ring-3 ${isActive ? "active" : ""}"></div>
        <button
          class="mic-btn ${isActive ? "active" : ""}"
          ?disabled=${this.disabled}
          @click=${this._handleClick}
          aria-label=${isActive ? "Stop listening" : "Start listening"}
        >
          ${icons.mic}
        </button>
      </div>
      <div class="voice-status ${this.state}" aria-live="polite">${this._statusText}</div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-voice-orb": ScVoiceOrb;
  }
}
