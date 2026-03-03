import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-modal")
export class ScModal extends LitElement {
  static override styles = css`
    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 9999;
      background: rgba(0, 0, 0, 0.5);
      backdrop-filter: blur(4px);
      -webkit-backdrop-filter: blur(4px);
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-lg);
      box-sizing: border-box;
    }

    .panel {
      max-width: 480px;
      width: 100%;
      background: var(--sc-bg-overlay);
      border-radius: var(--sc-radius-xl);
      box-shadow: var(--sc-shadow-lg);
      animation: sc-scale-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-md) var(--sc-space-lg);
      border-bottom: 1px solid var(--sc-border);
    }

    .heading {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: 0;
    }

    .close-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 24px;
      height: 24px;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition: color var(--sc-duration-fast);
    }

    .close-btn:hover {
      color: var(--sc-text);
    }

    .body {
      padding: var(--sc-space-lg);
    }
  `;

  @property({ type: Boolean }) open = false;
  @property({ type: String }) heading = "";

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open") && this.open) {
      requestAnimationFrame(() => this._focusCloseButton());
    }
  }

  private _focusCloseButton(): void {
    const btn = this.renderRoot.querySelector<HTMLButtonElement>(".close-btn");
    btn?.focus();
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      this._close();
    }
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open) return nothing;

    return html`
      <div
        class="backdrop"
        role="dialog"
        aria-modal="true"
        aria-labelledby=${this.heading ? "modal-heading" : undefined}
        @click=${this._onBackdropClick}
        @keydown=${this._onKeyDown}
      >
        <div class="panel" @click=${(e: MouseEvent) => e.stopPropagation()}>
          <header class="header">
            ${this.heading
              ? html`<h2 id="modal-heading" class="heading">${this.heading}</h2>`
              : nothing}
            <button class="close-btn" aria-label="Close" @click=${this._close}>×</button>
          </header>
          <div class="body">
            <slot></slot>
          </div>
        </div>
      </div>
    `;
  }
}
