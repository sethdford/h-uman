import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

export type SelectOption = { value: string; label: string };

type SelectSize = "sm" | "md" | "lg";

@customElement("hu-select")
export class ScSelect extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) options: SelectOption[] = [];
  @property({ type: String }) placeholder = "";
  @property({ type: String }) label = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: String }) size: SelectSize = "md";

  @state() private _selectId = `hu-select-${Math.random().toString(36).slice(2, 11)}`;

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

    .select-wrap {
      position: relative;
      display: inline-block;
      width: 100%;
    }

    select {
      width: 100%;
      appearance: none;
      -webkit-appearance: none;
      -moz-appearance: none;
      box-sizing: border-box;
      font-family: var(--hu-font);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      outline: none;
      cursor: pointer;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
      padding-right: var(--hu-space-xl);
    }

    select:hover:not(:disabled):not(:focus) {
      border-color: var(--hu-border);
    }

    select:focus-visible {
      border-color: var(--hu-accent);
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    select.error {
      border-color: var(--hu-error);
    }

    select:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    select.size-sm {
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      padding-right: var(--hu-space-xl);
      min-height: calc(var(--hu-input-min-height) - var(--hu-space-sm));
    }

    select.size-md {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      padding-right: var(--hu-space-xl);
      min-height: var(--hu-input-min-height);
    }

    select.size-lg {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      padding-right: var(--hu-space-xl);
      min-height: calc(var(--hu-input-min-height) + var(--hu-space-sm));
    }

    .chevron {
      position: absolute;
      right: var(--hu-space-sm);
      top: 50%;
      transform: translateY(-50%);
      pointer-events: none;
      color: var(--hu-text-muted);
    }

    .chevron svg {
      width: 1rem;
      height: 1rem;
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
      margin-top: var(--hu-space-xs);
    }
  `;

  private _onChange(e: Event): void {
    const target = e.target as HTMLSelectElement;
    this.value = target.value;
    this.dispatchEvent(
      new CustomEvent("hu-change", {
        bubbles: true,
        composed: true,
        detail: { value: target.value },
      }),
    );
  }

  override render() {
    const errorId = this.error ? `${this._selectId}-error` : undefined;
    return html`
      <div class="wrapper">
        ${this.label
          ? html`<label class="label" for=${this._selectId}>${this.label}</label>`
          : null}
        <div class="select-wrap">
          <select
            id=${this._selectId}
            class="${this.error ? "error " : ""}size-${this.size}"
            .value=${this.value}
            ?disabled=${this.disabled}
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${errorId ?? undefined}
            aria-label=${this.label || this.placeholder || "Select"}
            @change=${this._onChange}
          >
            ${this.placeholder
              ? html`<option value="" disabled>${this.placeholder}</option>`
              : null}
            ${this.options.map((opt) => html`<option value=${opt.value}>${opt.label}</option>`)}
          </select>
          <span class="chevron" aria-hidden="true">${icons["caret-down"]}</span>
        </div>
        ${this.error
          ? html`<span
              class="error-msg"
              id=${`${this._selectId}-error`}
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
    "hu-select": ScSelect;
  }
}
