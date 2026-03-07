import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import { ScToast } from "./sc-toast.js";
import "./sc-toast.js";

@customElement("sc-message-actions")
export class ScMessageActions extends LitElement {
  @property({ type: String }) role: "user" | "assistant" = "assistant";

  @property({ type: String }) content = "";

  @property({ type: Number }) index = -1;

  static override styles = css`
    :host {
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      position: absolute;
      top: -28px;
      right: var(--sc-space-sm);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      background: color-mix(
        in srgb,
        var(--sc-bg-surface) var(--sc-glass-standard-bg-opacity, 92%),
        transparent
      );
      backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius);
      box-shadow: var(--sc-shadow-sm);
      opacity: 0;
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
      z-index: 2;
    }

    :host(:focus-within) {
      opacity: 1;
    }

    .action-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 28px;
      height: 28px;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .action-btn:hover {
      color: var(--sc-text);
      background: var(--sc-bg-elevated);
    }

    .action-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .action-btn svg {
      width: 16px;
      height: 16px;
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        transition: none;
      }
      .action-btn {
        transition: none;
      }
    }
  `;

  private _onCopy(): void {
    if (!this.content) return;
    navigator.clipboard?.writeText(this.content).catch(() => {});
    ScToast.show({ message: "Copied to clipboard", variant: "success" });
    this.dispatchEvent(
      new CustomEvent("sc-copy", {
        bubbles: true,
        composed: true,
        detail: { content: this.content },
      }),
    );
  }

  private _onRetry(): void {
    this.dispatchEvent(
      new CustomEvent("sc-retry", {
        bubbles: true,
        composed: true,
        detail: { content: this.content, index: this.index },
      }),
    );
  }

  private _onRegenerate(): void {
    this.dispatchEvent(
      new CustomEvent("sc-regenerate", {
        bubbles: true,
        composed: true,
        detail: { content: this.content, index: this.index },
      }),
    );
  }

  override render() {
    return html`
      <button type="button" class="action-btn" aria-label="Copy" @click=${this._onCopy}>
        ${icons.copy}
      </button>
      ${this.role === "user"
        ? html`
            <button type="button" class="action-btn" aria-label="Retry" @click=${this._onRetry}>
              ${icons["arrow-clockwise"]}
            </button>
          `
        : html`
            <button
              type="button"
              class="action-btn"
              aria-label="Regenerate"
              @click=${this._onRegenerate}
            >
              ${icons.refresh}
            </button>
          `}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-message-actions": ScMessageActions;
  }
}
