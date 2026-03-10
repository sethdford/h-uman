import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { ScToast } from "./hu-toast.js";
import "./hu-toast.js";

@customElement("hu-message-actions")
export class ScMessageActions extends LitElement {
  @property({ type: String }) role: "user" | "assistant" = "assistant";

  @property({ type: String }) content = "";

  @property({ type: Number }) index = -1;

  @state() private _copied = false;

  static override styles = css`
    :host {
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      position: absolute;
      top: calc(var(--hu-space-xl) * -1); /* hu-lint-ok: action bar offset */
      right: var(--hu-space-sm);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-bg-overlay, var(--hu-bg-surface)) 65%, transparent);
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      border: 1px solid color-mix(in srgb, white 8%, transparent);
      border-radius: var(--hu-radius-full);
      box-shadow: var(--hu-shadow-sm);
      opacity: 0;
      transform: translateY(-4px);
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
      z-index: 2;
    }

    :host(:focus-within) {
      opacity: 1;
      transform: translateY(0);
    }

    .action-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .action-btn:hover {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }

    .action-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .action-btn.copied {
      color: var(--hu-success);
    }

    .action-btn svg {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
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
    this.dispatchEvent(
      new CustomEvent("hu-copy", {
        bubbles: true,
        composed: true,
        detail: { content: this.content },
      }),
    );
    navigator.clipboard
      ?.writeText(this.content)
      .then(() => {
        this._copied = true;
        setTimeout(() => {
          this._copied = false;
        }, 1500);
        ScToast.show({ message: "Copied to clipboard", variant: "success" });
      })
      .catch(() => {
        /* clipboard API unavailable */
      });
  }

  private _onEdit(): void {
    this.dispatchEvent(
      new CustomEvent("hu-edit", {
        bubbles: true,
        composed: true,
        detail: { content: this.content, index: this.index },
      }),
    );
  }

  private _onRetry(): void {
    this.dispatchEvent(
      new CustomEvent("hu-retry", {
        bubbles: true,
        composed: true,
        detail: { content: this.content, index: this.index },
      }),
    );
  }

  private _onRegenerate(): void {
    this.dispatchEvent(
      new CustomEvent("hu-regenerate", {
        bubbles: true,
        composed: true,
        detail: { content: this.content, index: this.index },
      }),
    );
  }

  override render() {
    return html`
      <button
        type="button"
        class="action-btn ${this._copied ? "copied" : ""}"
        aria-label="Copy"
        title=${this._copied ? "Copied!" : "Copy"}
        @click=${this._onCopy}
      >
        ${this._copied ? icons.check : icons.copy}
      </button>
      ${this.role === "user"
        ? html`
            <button
              type="button"
              class="action-btn"
              aria-label="Edit"
              title="Edit"
              @click=${this._onEdit}
            >
              ${icons["pencil-simple"]}
            </button>
            <button
              type="button"
              class="action-btn"
              aria-label="Retry"
              title="Retry"
              @click=${this._onRetry}
            >
              ${icons["arrow-clockwise"]}
            </button>
          `
        : html`
            <button
              type="button"
              class="action-btn"
              aria-label="Regenerate"
              title="Regenerate"
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
    "hu-message-actions": ScMessageActions;
  }
}
