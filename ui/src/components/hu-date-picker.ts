import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-date-picker")
export class ScDatePicker extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  @property({ type: String }) min = "";
  @property({ type: String }) max = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";

  @state() private _inputId = `hu-date-picker-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
    }

    .input-wrap {
      position: relative;
      display: flex;
      align-items: center;
    }

    .icon {
      position: absolute;
      left: var(--hu-space-sm);
      top: 50%;
      transform: translateY(-50%);
      color: var(--hu-text-muted);
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
      font-family: var(--hu-font);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      outline: none;
      padding: var(--hu-space-sm) var(--hu-space-md);
      padding-left: calc(var(--hu-space-md) + 1.125rem + var(--hu-space-xs));
      min-height: var(--hu-input-min-height);
      font-size: var(--hu-text-base);
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    input:hover:not(:disabled):not(:focus) {
      border-color: var(--hu-text-faint);
    }

    input:focus-visible {
      border-color: var(--hu-accent);
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    input.error {
      border-color: var(--hu-error);
    }

    input:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
      margin-top: var(--hu-space-xs);
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
      new CustomEvent("hu-change", {
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
    "hu-date-picker": ScDatePicker;
  }
}
