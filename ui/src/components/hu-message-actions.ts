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

  @property({ type: Boolean, attribute: "newly-sent" }) newlySent = false;

  @state() private _copied = false;

  @state() private _undoAvailable = false;

  private _undoTimer = 0;

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
      border: 1px solid color-mix(in srgb, var(--hu-color-white) 8%, transparent);
      border-radius: var(--hu-radius-full);
      box-shadow: var(--hu-shadow-sm);
      opacity: 0;
      transform: translateY(-4px);
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
      z-index: 2;
    }

    :host(:focus-within),
    :host([data-undo]) {
      opacity: 1;
      transform: translateY(0);
    }

    .action-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      min-width: 2.75rem;
      min-height: 2.75rem;
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

    @keyframes hu-undo-fade {
      0%,
      80% {
        opacity: 1;
      }
      100% {
        opacity: 0;
      }
    }

    .undo-btn {
      font-family: var(--hu-font);
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-warning) 12%, transparent);
      border: 1px solid color-mix(in srgb, var(--hu-warning) 25%, transparent);
      border-radius: var(--hu-radius-full);
      cursor: pointer;
      min-width: auto;
      min-height: auto;
      animation: hu-undo-fade 5s var(--hu-ease-out) forwards; /* hu-lint-ok: functional timeout */
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .undo-btn:hover {
      color: var(--hu-text);
      background: color-mix(in srgb, var(--hu-warning) 20%, transparent);
    }

    .undo-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        transition: none;
      }
      .action-btn {
        transition: none;
      }
      .undo-btn {
        animation: none;
      }
    }
  `;

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("newlySent")) {
      if (this.newlySent && this.role === "user") {
        clearTimeout(this._undoTimer);
        this._undoAvailable = true;
        this._undoTimer = window.setTimeout(() => {
          this._undoAvailable = false;
        }, 5000);
      } else {
        clearTimeout(this._undoTimer);
        this._undoAvailable = false;
      }
    }
  }

  override disconnectedCallback(): void {
    clearTimeout(this._undoTimer);
    super.disconnectedCallback();
  }

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

  private _onUndo(): void {
    clearTimeout(this._undoTimer);
    this._undoAvailable = false;
    this.dispatchEvent(
      new CustomEvent("hu-undo-send", {
        bubbles: true,
        composed: true,
        detail: { index: this.index },
      }),
    );
  }

  override render() {
    if (this._undoAvailable) {
      this.setAttribute("data-undo", "");
    } else {
      this.removeAttribute("data-undo");
    }

    if (this._undoAvailable) {
      return html`
        <button
          type="button"
          class="action-btn undo-btn"
          aria-label="Undo send"
          title="Undo send"
          @click=${this._onUndo}
        >
          Undo
        </button>
      `;
    }

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
      ${
        this.role === "user"
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
            `
      }
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-message-actions": ScMessageActions;
  }
}
