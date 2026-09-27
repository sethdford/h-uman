import { LitElement, html, css } from "lit";
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
  private _closedByConfirm = false;
  private _animationEndHandler = this._onAnimationEnd.bind(this);
  private _cancelHandler = this._onCancel.bind(this);
  private _closeHandler = this._onClose.bind(this);

  static override styles = css`
    dialog {
      position: fixed;
      inset: 0;
      z-index: var(--hu-z-modal-backdrop);
      margin: auto;
      padding: var(--hu-space-lg);
      border: none;
      background: transparent;
      max-width: var(--hu-modal-max-width, 400px);
      width: 100%;
      box-sizing: border-box;
    }

    dialog::backdrop {
      background: var(--hu-backdrop-overlay);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      animation: hu-backdrop-dim var(--hu-duration-normal) var(--hu-ease-out) both;
    }

    dialog.closing::backdrop {
      animation: hu-fade-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      dialog::backdrop {
        animation: none;
      }
      dialog.closing::backdrop {
        animation: none;
      }
    }

    dialog .panel {
      width: 100%;
      background: color-mix(in srgb, var(--hu-bg-overlay) 85%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      border: 1px solid var(--hu-glass-border-color);
      border-radius: var(--hu-radius-xl);
      box-shadow: var(--hu-shadow-xl);
      padding: var(--hu-space-lg);
    }

    dialog[open] .panel:not(.closing) {
      animation: hu-dialog-enter var(--hu-duration-normal)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    dialog .panel.closing {
      animation:
        hu-fade-out var(--hu-duration-fast) var(--hu-ease-in) forwards,
        hu-scale-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      dialog .panel {
        animation: none;
      }
      dialog .panel.closing {
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
      color: var(--hu-on-error);
    }

    .btn-confirm-danger:hover {
      background: var(--hu-error);
      filter: brightness(1.15);
    }

    @keyframes hu-backdrop-dim {
      from {
        background-color: transparent;
      }
      to {
        background-color: color-mix(in srgb, var(--hu-bg) 60%, transparent);
      }
    }
    @keyframes hu-dialog-enter {
      from {
        opacity: 0;
        transform: scale(0.95) translateY(8px);
      }
      to {
        opacity: 1;
        transform: scale(1) translateY(0);
      }
    }
  `;

  override firstUpdated(): void {
    const dialog = this.renderRoot.querySelector("dialog");
    if (dialog) {
      dialog.addEventListener("cancel", this._cancelHandler);
      dialog.addEventListener("close", this._closeHandler);
      dialog.addEventListener("animationend", this._animationEndHandler);
    }
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      const dialog = this.renderRoot.querySelector("dialog");
      if (!dialog) return;
      if (this.open) {
        this._closing = false;
        this._closedByConfirm = false;
        try {
          dialog.showModal();
        } catch {
          dialog.setAttribute("open", "");
        }
      } else if (changedProperties.get("open") === true) {
        this._startClosing(dialog);
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    const dialog = this.renderRoot.querySelector("dialog");
    if (dialog) {
      dialog.removeEventListener("cancel", this._cancelHandler);
      dialog.removeEventListener("close", this._closeHandler);
      dialog.removeEventListener("animationend", this._animationEndHandler);
    }
  }

  private _startClosing(dialog: HTMLDialogElement): void {
    if (this._closing) return;
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
      dialog.close();
      return;
    }
    this._closing = true;
  }

  private _onAnimationEnd(e: AnimationEvent): void {
    if (e.target instanceof HTMLElement && !e.target.classList.contains("panel")) return;
    const dialog = this.renderRoot.querySelector("dialog");
    if (!dialog || !this._closing) return;
    this._closing = false;
    dialog.close();
  }

  private _onCancel(e: Event): void {
    e.preventDefault();
    this.dispatchEvent(new CustomEvent("hu-cancel", { bubbles: true, composed: true }));
    this._startClosing(this.renderRoot.querySelector("dialog")!);
  }

  private _onClose(): void {
    this.open = false;
  }

  private _confirm(): void {
    this._closedByConfirm = true;
    this.dispatchEvent(new CustomEvent("hu-confirm", { bubbles: true, composed: true }));
    this._startClosing(this.renderRoot.querySelector("dialog")!);
  }

  private _cancel(): void {
    this.dispatchEvent(new CustomEvent("hu-cancel", { bubbles: true, composed: true }));
    this._startClosing(this.renderRoot.querySelector("dialog")!);
  }

  override render() {
    const titleId = "hu-dialog-title";
    const descId = "hu-dialog-desc";
    const confirmClass = this.variant === "danger" ? "btn-confirm-danger" : "btn-confirm-default";

    return html`
      <dialog
        class=${this._closing ? "closing" : ""}
        aria-labelledby=${titleId}
        aria-describedby=${descId}
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
      </dialog>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-dialog": ScDialog;
  }
}
