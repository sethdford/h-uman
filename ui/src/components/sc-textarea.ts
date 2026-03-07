import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type ResizeMode = "none" | "vertical" | "both";

@customElement("sc-textarea")
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

  @state() private _textareaId = `sc-textarea-${Math.random().toString(36).slice(2, 11)}`;

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

    .textarea-wrap {
      position: relative;
    }

    textarea {
      width: 100%;
      box-sizing: border-box;
      font-family: var(--sc-font);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      outline: none;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
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
      color: var(--sc-text-muted);
    }

    textarea:hover:not(:disabled):not(:focus) {
      border-color: var(--sc-text-faint);
    }

    textarea:focus-visible {
      border-color: var(--sc-accent);
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    textarea.error {
      border-color: var(--sc-error);
    }

    textarea:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    textarea {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      min-height: var(--sc-input-min-height);
    }

    .footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: var(--sc-space-sm);
    }

    .error-msg {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
    }

    .counter {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-faint);
    }
  `;

  private _onInput(e: Event): void {
    const target = e.target as HTMLTextAreaElement;
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
    const target = e.target as HTMLTextAreaElement;
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
    "sc-textarea": ScTextarea;
  }
}
