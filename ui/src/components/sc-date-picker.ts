import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("sc-date-picker")
export class ScDatePicker extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  @property({ type: String }) min = "";
  @property({ type: String }) max = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";

  @state() private _inputId = `sc-date-picker-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }

    .label {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
    }

    .input-wrap {
      position: relative;
      display: flex;
      align-items: center;
    }

    .icon {
      position: absolute;
      left: var(--sc-space-sm);
      top: 50%;
      transform: translateY(-50%);
      color: var(--sc-text-muted);
      pointer-events: none;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .icon svg {
      width: 1.125rem;
      height: 1.125rem;
    }

    input {
      width: 100%;
      box-sizing: border-box;
      font-family: var(--sc-font);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      outline: none;
      padding: var(--sc-space-sm) var(--sc-space-md);
      padding-left: calc(var(--sc-space-md) + 1.125rem + var(--sc-space-xs));
      min-height: var(--sc-input-min-height);
      font-size: var(--sc-text-base);
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    input:hover:not(:disabled):not(:focus) {
      border-color: var(--sc-text-faint);
    }

    input:focus-visible {
      border-color: var(--sc-accent);
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    input.error {
      border-color: var(--sc-error);
    }

    input:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    .error-msg {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
      margin-top: var(--sc-space-xs);
    }
  `;

  private _toISODate(val: string): string {
    if (!val) return "";
    const d = new Date(val);
    if (isNaN(d.getTime())) return val;
    return d.toISOString().slice(0, 10);
  }

  private _onChange(e: Event): void {
    const target = e.target as HTMLInputElement;
    const val = target.value;
    this.value = val;
    this.dispatchEvent(
      new CustomEvent("sc-change", {
        bubbles: true,
        composed: true,
        detail: { value: val },
      }),
    );
  }

  override render() {
    const displayValue = this.value ? this._toISODate(this.value) : "";
    const errorId = this.error ? `${this._inputId}-error` : undefined;
    return html`
      <div class="wrapper">
        ${this.label ? html`<label class="label" for=${this._inputId}>${this.label}</label>` : null}
        <div class="input-wrap">
          <span class="icon" aria-hidden="true">${icons.calendar}</span>
          <input
            id=${this._inputId}
            type="date"
            .value=${displayValue}
            min=${this.min || undefined}
            max=${this.max || undefined}
            ?disabled=${this.disabled}
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${errorId ?? undefined}
            aria-label=${this.label ? undefined : this.ariaLabel || undefined}
            class=${this.error ? "error" : ""}
            @change=${this._onChange}
          />
        </div>
        ${this.error
          ? html`<span class="error-msg" id=${errorId} role="alert" aria-live="polite"
              >${this.error}</span
            >`
          : null}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-date-picker": ScDatePicker;
  }
}
