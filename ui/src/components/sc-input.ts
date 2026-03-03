import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type InputSize = "sm" | "md" | "lg";

@customElement("sc-input")
export class ScInput extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "";
  @property({ type: String }) label = "";
  @property({ type: String }) type = "text";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: String }) size: InputSize = "md";

  @state() private _inputId = `sc-input-${Math.random().toString(36).slice(2, 11)}`;

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
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    input::placeholder {
      color: var(--sc-text-muted);
    }

    input:hover:not(:disabled):not(:focus) {
      border-color: var(--sc-text-faint);
    }

    input:focus-within {
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 var(--sc-focus-ring-width) var(--sc-focus-ring);
    }

    input.error {
      border-color: var(--sc-error);
    }

    input:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    input.size-sm {
      font-size: var(--sc-text-sm);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      min-height: calc(var(--sc-input-min-height) - var(--sc-space-sm));
    }

    input.size-md {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      min-height: var(--sc-input-min-height);
    }

    input.size-lg {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      min-height: calc(var(--sc-input-min-height) + var(--sc-space-sm));
    }

    .error-msg {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
      margin-top: var(--sc-space-xs);
    }
  `;

  private _onInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this.value = target.value;
    this.dispatchEvent(
      new CustomEvent("sc-input", {
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
      new CustomEvent("sc-change", {
        bubbles: true,
        composed: true,
        detail: { value: target.value },
      }),
    );
  }

  override render() {
    const errorId = this.error ? "sc-input-error" : undefined;
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
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${errorId ?? undefined}
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
    "sc-input": ScInput;
  }
}
