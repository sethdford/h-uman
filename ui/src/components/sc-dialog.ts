import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type DialogVariant = "default" | "danger";

@customElement("sc-dialog")
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
      z-index: var(--sc-z-modal-backdrop);
      background: var(--sc-backdrop-overlay);
      backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-lg);
      box-sizing: border-box;
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .backdrop.closing {
      animation: sc-fade-out var(--sc-duration-fast) var(--sc-ease-in) forwards;
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
      max-width: var(--sc-modal-max-width, 400px);
      background: color-mix(in srgb, var(--sc-bg-overlay) 85%, transparent);
      backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      border: 1px solid var(--sc-glass-border-color);
      border-radius: var(--sc-radius-xl);
      box-shadow: var(--sc-shadow-xl);
      padding: var(--sc-space-lg);
      animation: sc-bounce-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .panel.closing {
      animation:
        sc-fade-out var(--sc-duration-fast) var(--sc-ease-in) forwards,
        sc-scale-out var(--sc-duration-fast) var(--sc-ease-in) forwards;
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
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: 0 0 var(--sc-space-sm);
      font-family: var(--sc-font);
    }

    .message {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      margin: 0 0 var(--sc-space-lg);
      font-family: var(--sc-font);
      line-height: var(--sc-leading-normal);
    }

    .actions {
      display: flex;
      gap: var(--sc-space-sm);
      justify-content: flex-end;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      border-radius: var(--sc-radius);
      border: none;
      cursor: pointer;
      transition:
        background-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-normal) var(--sc-spring-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out);
    }

    .btn:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .btn:active {
      transform: scale(0.97);
    }

    .btn-cancel {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
    }

    .btn-cancel:hover {
      background: var(--sc-hover-overlay);
    }

    .btn-confirm-default {
      background: var(--sc-accent);
      color: var(--sc-on-accent);
    }

    .btn-confirm-default:hover {
      background: var(--sc-accent-hover);
    }

    .btn-confirm-danger {
      background: var(--sc-error);
      color: var(--sc-on-accent);
    }

    .btn-confirm-danger:hover {
      background: var(--sc-error);
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
    this.dispatchEvent(new CustomEvent("sc-confirm", { bubbles: true, composed: true }));
  }

  private _cancel(): void {
    this.dispatchEvent(new CustomEvent("sc-cancel", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open && !this._closing) return nothing;

    const titleId = "sc-dialog-title";
    const descId = "sc-dialog-desc";
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
          class="panel sc-entry-scale ${this._closing ? "closing" : ""}"
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
    "sc-dialog": ScDialog;
  }
}
