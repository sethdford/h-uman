import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type ResizeMode = "none" | "vertical" | "both";

@customElement("hu-textarea")
export class ScTextarea extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "";
  @property({ type: String }) label = "";
  @property({ type: Number }) rows = 4;
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: Number }) maxlength = 0;
  @property({ type: String }) resize: ResizeMode = "vertical";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";
  /** @deprecated Use ariaLabel or aria-label attribute instead */
  @property({ type: String }) accessibleLabel = "";

  @state() private _textareaId = `hu-textarea-${Math.random().toString(36).slice(2, 11)}`;

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

    .textarea-wrap {
      position: relative;
    }

    textarea {
      width: 100%;
      box-sizing: border-box;
      font-family: var(--hu-font);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      outline: none;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    textarea.resize-none {
      resize: none;
    }

    textarea.resize-vertical {
      resize: vertical;
    }

    textarea.resize-both {
      resize: both;
    }

    textarea::placeholder {
      color: var(--hu-text-muted);
    }

    textarea:hover:not(:disabled):not(:focus) {
      border-color: var(--hu-text-faint);
    }

    textarea:focus-visible {
      border-color: var(--hu-accent);
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    textarea.error {
      border-color: var(--hu-error);
    }

    textarea:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    textarea {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      min-height: var(--hu-input-min-height);
    }

    .footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
    }

    .counter {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-faint);
    }
  `;

  private _onInput(e: Event): void {
    const target = e.target as HTMLTextAreaElement;
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
    const target = e.target as HTMLTextAreaElement;
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
    const errorId = this.error ? `${this._textareaId}-error` : undefined;
    const showCounter = this.maxlength > 0;
    const currentLength = this.value.length;

    return html`
      <div class="wrapper">
        ${this.label
          ? html`<label class="label" for=${this._textareaId}>${this.label}</label>`
          : null}
        <div class="textarea-wrap">
          <textarea
            id=${this._textareaId}
            class="${this.error ? "error " : ""}resize-${this.resize}"
            rows=${this.rows}
            .value=${this.value}
            placeholder=${this.placeholder}
            ?disabled=${this.disabled}
            maxlength=${this.maxlength || undefined}
            aria-label=${this.label
              ? undefined
              : this.ariaLabel || this.accessibleLabel || undefined}
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${errorId ?? undefined}
            @input=${this._onInput}
            @change=${this._onChange}
          ></textarea>
        </div>
        <div class="footer">
          <span>
            ${this.error
              ? html`<span class="error-msg" id=${errorId} role="alert" aria-live="polite"
                  >${this.error}</span
                >`
              : null}
          </span>
          ${showCounter
            ? html`<span class="counter" aria-live="polite"
                >${currentLength}/${this.maxlength}</span
              >`
            : null}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-textarea": ScTextarea;
  }
}
