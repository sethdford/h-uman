import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("hu-checkbox")
export class ScCheckbox extends LitElement {
  @property({ type: Boolean }) checked = false;
  @property({ type: Boolean }) indeterminate = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  @property({ type: String }) error = "";

  @state() private _checkboxId = `hu-checkbox-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: inline-block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .row {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-sm);
      cursor: pointer;
    }

    .row.disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .box {
      flex-shrink: 0;
      width: 1.125rem;
      height: 1.125rem;
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      display: flex;
      align-items: center;
      justify-content: center;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .row:not(.disabled):hover .box {
      border-color: var(--hu-text-muted);
    }

    .box.checked {
      background: var(--hu-accent);
      border-color: var(--hu-accent);
    }

    .box.indeterminate {
      background: var(--hu-accent);
      border-color: var(--hu-accent);
    }

    .row:focus-visible .box {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .row:focus-visible .box.checked,
    .row:focus-visible .box.indeterminate {
      box-shadow: 0 0 0 2px var(--hu-accent-subtle);
    }

    .check {
      width: 0.375rem;
      height: 0.625rem;
      border: 2px solid var(--hu-on-accent);
      border-top: 0;
      border-left: 0;
      transform: rotate(45deg) translateY(-1px);
    }

    .dash {
      width: 0.5rem;
      height: 2px;
      background: var(--hu-on-accent);
    }

    .label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
      margin-left: calc(1.125rem + var(--hu-space-sm));
    }

    @media (prefers-reduced-motion: reduce) {
      .box {
        transition: none;
      }
    }
  `;

  private _onClick(): void {
    if (this.disabled) return;
    this.indeterminate = false;
    this.checked = !this.checked;
    this.dispatchEvent(
      new CustomEvent("hu-change", {
        bubbles: true,
        composed: true,
        detail: { checked: this.checked },
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === " ") {
      e.preventDefault();
      this._onClick();
    }
  }

  override render() {
    const ariaChecked = this.indeterminate ? "mixed" : this.checked;
    return html`
      <div class="wrapper">
        <div
          class="row ${this.disabled ? "disabled" : ""}"
          role="checkbox"
          aria-checked=${ariaChecked}
          aria-disabled=${this.disabled}
          aria-invalid=${this.error ? "true" : "false"}
          aria-describedby=${this.error ? `${this._checkboxId}-error` : undefined}
          aria-label=${this.label ? undefined : this.ariaLabel || undefined}
          tabindex=${this.disabled ? -1 : 0}
          @click=${this._onClick}
          @keydown=${this._onKeyDown}
        >
          <span
            class="box ${this.checked ? "checked" : ""} ${this.indeterminate
              ? "indeterminate"
              : ""}"
            aria-hidden="true"
          >
            ${this.indeterminate
              ? html`<span class="dash"></span>`
              : this.checked
                ? html`<span class="check"></span>`
                : null}
          </span>
          ${this.label ? html`<span class="label">${this.label}</span>` : null}
        </div>
        ${this.error
          ? html`<span
              class="error-msg"
              id=${`${this._checkboxId}-error`}
              role="alert"
              aria-live="polite"
              >${this.error}</span
            >`
          : null}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-checkbox": ScCheckbox;
  }
}
