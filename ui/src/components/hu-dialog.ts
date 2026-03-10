import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type DialogVariant = "default" | "danger";

@customElement("hu-dialog")
export class ScDialog extends LitElement {
  @property({ type: Boolean }) open = false;
  @property({ type: String }) title = "";
  @property({ type: String }) message = "";
  @property({ type: String }) confirmLabel = "Confirm";
  @property({ type: String }) cancelLabel = "Cancel";
  @property({ type: String }) variant: DialogVariant = "default";

  @state() private _closing = false;
  private _closeTimeout: ReturnType<typeof setTimeout> | null = null;
  private _focusableSelector =
    'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
  private _keyHandler = this._onKeyDown.bind(this);

  static override styles = css`
    .backdrop {
      position: fixed;
      inset: 0;
      z-index: var(--hu-z-modal-backdrop);
      background: var(--hu-backdrop-overlay);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-lg);
      box-sizing: border-box;
      animation: hu-fade-in var(--hu-duration-normal) var(--hu-ease-out);
    }

    .backdrop.closing {
      animation: hu-fade-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      .backdrop {
        animation: none;
      }
      .backdrop.closing {
        animation: none;
      }
    }

    .panel {
      width: 100%;
      max-width: var(--hu-modal-max-width, 400px);
      background: color-mix(in srgb, var(--hu-bg-overlay) 85%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      border: 1px solid var(--hu-glass-border-color);
      border-radius: var(--hu-radius-xl);
      box-shadow: var(--hu-shadow-xl);
      padding: var(--hu-space-lg);
      animation: hu-bounce-in var(--hu-duration-normal) var(--hu-ease-out);
    }

    .panel.closing {
      animation:
        hu-fade-out var(--hu-duration-fast) var(--hu-ease-in) forwards,
        hu-scale-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      .panel {
        animation: none;
      }
      .panel.closing {
        animation: none;
      }
    }

    .title {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-sm);
      font-family: var(--hu-font);
    }

    .message {
      font-size: var(--hu-text-base);
      color: var(--hu-text-muted);
      margin: 0 0 var(--hu-space-lg);
      font-family: var(--hu-font);
      line-height: var(--hu-leading-normal);
    }

    .actions {
      display: flex;
      gap: var(--hu-space-sm);
      justify-content: flex-end;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      border-radius: var(--hu-radius);
      border: none;
      cursor: pointer;
      transition:
        background-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-normal) var(--hu-spring-out),
        box-shadow var(--hu-duration-normal) var(--hu-ease-out);
    }

    .btn:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .btn:active {
      transform: scale(0.97);
    }

    .btn-cancel {
      background: var(--hu-bg-elevated);
      color: var(--hu-text);
      border: 1px solid var(--hu-border);
    }

    .btn-cancel:hover {
      background: var(--hu-hover-overlay);
    }

    .btn-confirm-default {
      background: var(--hu-accent);
      color: var(--hu-on-accent);
    }

    .btn-confirm-default:hover {
      background: var(--hu-accent-hover);
    }

    .btn-confirm-danger {
      background: var(--hu-error);
      color: var(--hu-on-accent);
    }

    .btn-confirm-danger:hover {
      background: var(--hu-error);
      filter: brightness(1.15);
    }
  `;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this._closing = false;
        if (this._closeTimeout) {
          clearTimeout(this._closeTimeout);
          this._closeTimeout = null;
        }
        document.addEventListener("keydown", this._keyHandler);
        requestAnimationFrame(() => this._trapFocus());
      } else if (changedProperties.get("open") === true) {
        this._closing = true;
        this._closeTimeout = setTimeout(() => {
          this._closing = false;
          this._closeTimeout = null;
          document.removeEventListener("keydown", this._keyHandler);
        }, 200);
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this._closeTimeout) clearTimeout(this._closeTimeout);
    document.removeEventListener("keydown", this._keyHandler);
  }

  private _getFocusableElements(): HTMLElement[] {
    const root = this.renderRoot;
    return Array.from(root.querySelectorAll<HTMLElement>(this._focusableSelector));
  }

  private _trapFocus(): void {
    const focusable = this._getFocusableElements();
    if (focusable.length > 0) {
      focusable[0].focus();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.open) return;
    if (e.key === "Escape") {
      e.preventDefault();
      this._cancel();
      return;
    }
    if (e.key !== "Tab") return;
    const focusable = this._getFocusableElements();
    if (focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (e.shiftKey) {
      if (document.activeElement === first) {
        e.preventDefault();
        last.focus();
      }
    } else if (document.activeElement === last) {
      e.preventDefault();
      first.focus();
    }
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._cancel();
    }
  }

  private _confirm(): void {
    this.dispatchEvent(new CustomEvent("hu-confirm", { bubbles: true, composed: true }));
  }

  private _cancel(): void {
    this.dispatchEvent(new CustomEvent("hu-cancel", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open && !this._closing) return nothing;

    const titleId = "hu-dialog-title";
    const descId = "hu-dialog-desc";
    const confirmClass = this.variant === "danger" ? "btn-confirm-danger" : "btn-confirm-default";

    return html`
      <div
        class="backdrop ${this._closing ? "closing" : ""}"
        role="alertdialog"
        aria-modal="true"
        aria-labelledby=${titleId}
        aria-describedby=${descId}
        @click=${this._onBackdropClick}
      >
        <div
          class="panel hu-entry-scale ${this._closing ? "closing" : ""}"
          @click=${(e: MouseEvent) => e.stopPropagation()}
        >
          <h2 id=${titleId} class="title">${this.title}</h2>
          <p id=${descId} class="message">${this.message}</p>
          <div class="actions">
            <button type="button" class="btn btn-cancel" @click=${this._cancel}>
              ${this.cancelLabel}
            </button>
            <button type="button" class="btn ${confirmClass}" @click=${this._confirm}>
              ${this.confirmLabel}
            </button>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-dialog": ScDialog;
  }
}
