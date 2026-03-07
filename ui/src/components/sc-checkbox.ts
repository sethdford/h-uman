import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("sc-checkbox")
export class ScCheckbox extends LitElement {
  @property({ type: Boolean }) checked = false;
  @property({ type: Boolean }) indeterminate = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  @property({ type: String }) error = "";

  @state() private _checkboxId = `sc-checkbox-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: inline-block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }

    .row {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-sm);
      cursor: pointer;
    }

    .row.disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    .box {
      flex-shrink: 0;
      width: 1.125rem;
      height: 1.125rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-sm);
      display: flex;
      align-items: center;
      justify-content: center;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        background-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }

    .row:not(.disabled):hover .box {
      border-color: var(--sc-text-muted);
    }

    .box.checked {
      background: var(--sc-accent);
      border-color: var(--sc-accent);
    }

    .box.indeterminate {
      background: var(--sc-accent);
      border-color: var(--sc-accent);
    }

    .row:focus-visible .box {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .row:focus-visible .box.checked,
    .row:focus-visible .box.indeterminate {
      box-shadow: 0 0 0 2px var(--sc-accent-subtle);
    }

    .check {
      width: 0.375rem;
      height: 0.625rem;
      border: 2px solid var(--sc-on-accent);
      border-top: 0;
      border-left: 0;
      transform: rotate(45deg) translateY(-1px);
    }

    .dash {
      width: 0.5rem;
      height: 2px;
      background: var(--sc-on-accent);
    }

    .label {
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
    }

    .error-msg {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
      margin-left: calc(1.125rem + var(--sc-space-sm));
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
      new CustomEvent("sc-change", {
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
    "sc-checkbox": ScCheckbox;
  }
}
