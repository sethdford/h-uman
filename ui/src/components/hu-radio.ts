import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export type RadioOption = {
  value: string;
  label: string;
  disabled?: boolean;
};

@customElement("hu-radio")
export class ScRadio extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) options: RadioOption[] = [];
  @property({ type: String }) name = "";
  @property({ type: String }) label = "";
  @property({ type: Boolean }) disabled = false;

  @state() private _focusedIndex = 0;

  static override styles = css`
    :host {
      display: block;
    }

    .group {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
    }

    .option {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      cursor: pointer;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }

    .option:has(input:disabled) {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .radio-wrap {
      flex-shrink: 0;
      width: 1.125rem;
      height: 1.125rem;
      border: 2px solid var(--hu-border);
      border-radius: var(--hu-radius-full);
      display: flex;
      align-items: center;
      justify-content: center;
      transition: border-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .option:hover:not(:has(input:disabled)) .radio-wrap {
      border-color: var(--hu-text-muted);
    }

    .option input:checked + .radio-wrap {
      border-color: var(--hu-accent);
    }

    .option input:checked + .radio-wrap::after {
      content: "";
      width: 0.5rem;
      height: 0.5rem;
      background: var(--hu-accent);
      border-radius: var(--hu-radius-full);
    }

    .option input:focus-visible + .radio-wrap {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    input {
      position: absolute;
      width: 1px;
      height: 1px;
      padding: 0;
      margin: -1px;
      overflow: hidden;
      clip: rect(0, 0, 0, 0);
      white-space: nowrap;
      border: 0;
    }

    @media (prefers-reduced-motion: reduce) {
      .radio-wrap {
        transition: none;
      }
    }
  `;

  private _onChange(e: Event): void {
    const target = e.target as HTMLInputElement;
    this.value = target.value;
    this._focusedIndex = this.options.findIndex((o) => o.value === target.value);
    this.dispatchEvent(
      new CustomEvent("hu-change", {
        bubbles: true,
        composed: true,
        detail: { value: target.value },
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    const enabled = this.options.filter((o) => !o.disabled && !this.disabled);
    if (enabled.length === 0) return;

    let nextIndex = this._focusedIndex;
    if (e.key === "ArrowDown" || e.key === "ArrowRight") {
      e.preventDefault();
      nextIndex = (this._focusedIndex + 1) % this.options.length;
      while (
        nextIndex !== this._focusedIndex &&
        (this.options[nextIndex].disabled || this.disabled)
      ) {
        nextIndex = (nextIndex + 1) % this.options.length;
      }
    } else if (e.key === "ArrowUp" || e.key === "ArrowLeft") {
      e.preventDefault();
      nextIndex = this._focusedIndex - 1;
      if (nextIndex < 0) nextIndex = this.options.length - 1;
      while (
        nextIndex !== this._focusedIndex &&
        (this.options[nextIndex].disabled || this.disabled)
      ) {
        nextIndex = nextIndex - 1;
        if (nextIndex < 0) nextIndex = this.options.length - 1;
      }
    }
    if (
      nextIndex >= 0 &&
      nextIndex < this.options.length &&
      !this.options[nextIndex].disabled &&
      !this.disabled
    ) {
      this._focusedIndex = nextIndex;
      this.value = this.options[nextIndex].value;
      const input = this.renderRoot.querySelectorAll('input[type="radio"]')[
        nextIndex
      ] as HTMLInputElement;
      input?.focus();
      this.dispatchEvent(
        new CustomEvent("hu-change", {
          bubbles: true,
          composed: true,
          detail: { value: this.options[nextIndex].value },
        }),
      );
    }
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("value") || changedProperties.has("options")) {
      const idx = this.options.findIndex((o) => o.value === this.value);
      this._focusedIndex = idx >= 0 ? idx : 0;
    }
  }

  override render() {
    const name = this.name || `hu-radio-${Math.random().toString(36).slice(2, 11)}`;
    return html`
      <div
        class="group"
        role="radiogroup"
        aria-label=${this.label || undefined}
        @keydown=${this._onKeyDown}
      >
        ${this.options.map((opt) => {
          const isChecked = opt.value === this.value;
          const isDisabled = opt.disabled || this.disabled;
          return html`
            <label class="option">
              <input
                type="radio"
                name=${name}
                value=${opt.value}
                ?checked=${isChecked}
                ?disabled=${isDisabled}
                role="radio"
                aria-checked=${isChecked}
                tabindex=${isChecked && !isDisabled ? 0 : -1}
                @change=${this._onChange}
              />
              <span class="radio-wrap"></span>
              <span>${opt.label}</span>
            </label>
          `;
        })}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-radio": ScRadio;
  }
}
