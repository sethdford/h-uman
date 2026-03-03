import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

export type SelectOption = { value: string; label: string };

type SelectSize = "sm" | "md" | "lg";

@customElement("sc-select")
export class ScSelect extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) options: SelectOption[] = [];
  @property({ type: String }) placeholder = "";
  @property({ type: String }) label = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: String }) size: SelectSize = "md";

  @state() private _selectId = `sc-select-${Math.random().toString(36).slice(2, 11)}`;

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
      font-family: var(--sc-font);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      outline: none;
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
      padding-right: var(--sc-space-xl);
    }

    select:hover:not(:disabled):not(:focus) {
      border-color: var(--sc-border);
    }

    select:focus {
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 var(--sc-focus-ring-width) var(--sc-focus-ring);
    }

    select.error {
      border-color: var(--sc-error);
    }

    select:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    select.size-sm {
      font-size: var(--sc-text-sm);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      padding-right: var(--sc-space-xl);
      min-height: calc(var(--sc-input-min-height) - var(--sc-space-sm));
    }

    select.size-md {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      padding-right: var(--sc-space-xl);
      min-height: var(--sc-input-min-height);
    }

    select.size-lg {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      padding-right: var(--sc-space-xl);
      min-height: calc(var(--sc-input-min-height) + var(--sc-space-sm));
    }

    .chevron {
      position: absolute;
      right: var(--sc-space-sm);
      top: 50%;
      transform: translateY(-50%);
      pointer-events: none;
      color: var(--sc-text-muted);
    }

    .chevron svg {
      width: 1rem;
      height: 1rem;
    }

    .error-msg {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
      margin-top: var(--sc-space-xs);
    }
  `;

  private _onChange(e: Event): void {
    const target = e.target as HTMLSelectElement;
    this.value = target.value;
    this.dispatchEvent(
      new CustomEvent("sc-change", {
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
            aria-label=${this.label || undefined}
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
    "sc-select": ScSelect;
  }
}
