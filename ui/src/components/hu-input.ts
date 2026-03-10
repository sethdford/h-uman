import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type InputSize = "sm" | "md" | "lg";

@customElement("hu-input")
export class ScInput extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "";
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  @property({ type: String }) type = "text";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: String }) size: InputSize = "md";
  @property({ type: Number }) min?: number;
  @property({ type: Number }) max?: number;
  @property({ type: Number }) step?: number;

  @state() private _inputId = `hu-input-${Math.random().toString(36).slice(2, 11)}`;

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
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    input::placeholder {
      color: var(--hu-text-muted);
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

    input.size-sm {
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      min-height: calc(var(--hu-input-min-height) - var(--hu-space-sm));
    }

    input.size-md {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      min-height: var(--hu-input-min-height);
    }

    input.size-lg {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      min-height: calc(var(--hu-input-min-height) + var(--hu-space-sm));
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
      margin-top: var(--hu-space-xs);
    }
  `;

  private _onInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this.value = target.value;
    this.dispatchEvent(
      new CustomEvent("hu-input", {
        bubbles: true,
        composed: true,
        detail: { value: target.value },
      }),
    );
  }

  private _onChange(e: Event): void {
    const target = e.target as HTMLInputElement;
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
    const errorId = this.error ? "hu-input-error" : undefined;
    return html`
      <div class="wrapper">
        ${this.label ? html`<label class="label" for=${this._inputId}>${this.label}</label>` : null}
        <div class="input-wrap">
          <input
            id=${this._inputId}
            class="${this.error ? "error " : ""}size-${this.size}"
            type=${this.type}
            .value=${this.value}
            placeholder=${this.placeholder}
            ?disabled=${this.disabled}
            min=${this.min ?? undefined}
            max=${this.max ?? undefined}
            step=${this.step ?? undefined}
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${errorId ?? undefined}
            aria-label=${this.label ? undefined : this.ariaLabel || undefined}
            @input=${this._onInput}
            @change=${this._onChange}
          />
        </div>
        ${this.error
          ? html`<span
              class="error-msg"
              id=${`${this._inputId}-error`}
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
    "hu-input": ScInput;
  }
}
