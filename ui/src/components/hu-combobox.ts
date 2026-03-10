import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export type ComboboxOption = { value: string; label: string };

@customElement("hu-combobox")
export class ScCombobox extends LitElement {
  @property({ type: Array }) options: ComboboxOption[] = [];
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "";
  @property({ type: Boolean, attribute: "free-text" }) freeText = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) error = "";
  @property({ type: String }) label = "";
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "";

  @state() private _open = false;
  @state() private _activeIndex = -1;
  @state() private _inputId = `hu-combobox-${Math.random().toString(36).slice(2, 11)}`;
  @state() private _listboxId = `hu-combobox-list-${Math.random().toString(36).slice(2, 11)}`;

  private _blurTimeout = 0;

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

    input {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      min-height: var(--hu-input-min-height);
    }

    .dropdown {
      position: absolute;
      top: 100%;
      left: 0;
      right: 0;
      margin-top: var(--hu-space-2xs);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      box-shadow: var(--hu-shadow-lg);
      max-height: 240px;
      overflow-y: auto;
      z-index: 20;
    }

    .option {
      display: block;
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
      color: var(--hu-text);
      cursor: pointer;
      transition: background var(--hu-duration-fast);
      border: none;
      background: transparent;
      width: 100%;
      text-align: left;
    }

    .option:hover,
    .option[aria-selected="true"] {
      background: var(--hu-hover-overlay);
    }

    .option .highlight {
      font-weight: var(--hu-weight-semibold);
    }

    .error-msg {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
      margin-top: var(--hu-space-xs);
    }

    @media (prefers-reduced-motion: reduce) {
      .option {
        transition: none;
      }
    }
  `;

  private get _displayValue(): string {
    if (!this.value) return "";
    const opt = this.options.find((o) => o.value === this.value);
    return opt ? opt.label : this.value;
  }

  private get _filteredOptions(): ComboboxOption[] {
    const q = (this._inputValue ?? this.value).trim().toLowerCase();
    if (!q) return this.options;
    return this.options.filter(
      (o) => o.label.toLowerCase().includes(q) || o.value.toLowerCase().includes(q),
    );
  }

  private _highlightLabel(label: string): ReturnType<typeof html> {
    const q = (this._inputValue ?? this.value).trim().toLowerCase();
    if (!q) return html`${label}`;
    const i = label.toLowerCase().indexOf(q);
    if (i < 0) return html`${label}`;
    const before = label.slice(0, i);
    const match = label.slice(i, i + q.length);
    const after = label.slice(i + q.length);
    return html`${before}<span class="highlight">${match}</span>${after}`;
  }

  @state() private _inputValue = "";

  override willUpdate(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("value")) {
      this._inputValue = this._displayValue;
    }
  }

  private _onInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this._inputValue = target.value;
    this.value = target.value;
    this._open = true;
    this._activeIndex = this._filteredOptions.length > 0 ? 0 : -1;
  }

  private _onFocus(): void {
    if (this.disabled) return;
    clearTimeout(this._blurTimeout);
    this._open = true;
    this._activeIndex = this._filteredOptions.length > 0 ? 0 : -1;
  }

  private _onBlur(): void {
    this._blurTimeout = window.setTimeout(() => {
      this._open = false;
      this._activeIndex = -1;
    }, 150);
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this._open = false;
      this._activeIndex = -1;
      (e.target as HTMLInputElement).blur();
      return;
    }
    if (e.key === "ArrowDown") {
      e.preventDefault();
      if (!this._open) {
        this._open = true;
        this._activeIndex = this._filteredOptions.length > 0 ? 0 : -1;
      } else {
        const opts = this._filteredOptions;
        this._activeIndex = opts.length > 0 ? (this._activeIndex + 1) % opts.length : -1;
      }
      return;
    }
    if (e.key === "ArrowUp") {
      e.preventDefault();
      if (!this._open) {
        this._open = true;
        const opts = this._filteredOptions;
        this._activeIndex = opts.length > 0 ? opts.length - 1 : -1;
      } else {
        const opts = this._filteredOptions;
        this._activeIndex =
          opts.length > 0 ? (this._activeIndex - 1 + opts.length) % opts.length : -1;
      }
      return;
    }
    if (e.key === "Enter") {
      e.preventDefault();
      const opts = this._filteredOptions;
      if (
        this._open &&
        opts.length > 0 &&
        this._activeIndex >= 0 &&
        this._activeIndex < opts.length
      ) {
        this._selectOption(opts[this._activeIndex]);
      } else if (this.freeText) {
        this._commitValue(this.value);
      }
      this._open = false;
      this._activeIndex = -1;
      return;
    }
  }

  private _selectOption(opt: ComboboxOption): void {
    this.value = opt.value;
    this._inputValue = opt.label;
    this._open = false;
    this._activeIndex = -1;
    this.dispatchEvent(
      new CustomEvent("hu-combobox-change", {
        bubbles: true,
        composed: true,
        detail: { value: opt.value },
      }),
    );
  }

  private _commitValue(val: string): void {
    const opt = this.options.find((o) => o.value === val || o.label === val);
    this.dispatchEvent(
      new CustomEvent("hu-combobox-change", {
        bubbles: true,
        composed: true,
        detail: { value: opt ? opt.value : val },
      }),
    );
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    clearTimeout(this._blurTimeout);
  }

  override render() {
    const filtered = this._filteredOptions;
    const activeId =
      this._open && filtered.length > 0 && this._activeIndex >= 0
        ? `${this._listboxId}-opt-${this._activeIndex}`
        : undefined;

    return html`
      <div class="wrapper">
        ${this.label ? html`<label class="label" for=${this._inputId}>${this.label}</label>` : null}
        <div class="input-wrap">
          <input
            id=${this._inputId}
            type="text"
            role="combobox"
            aria-expanded=${this._open}
            aria-autocomplete="list"
            aria-controls=${this._open ? this._listboxId : undefined}
            aria-activedescendant=${activeId ?? undefined}
            aria-invalid=${this.error ? "true" : "false"}
            aria-describedby=${this.error ? `${this._inputId}-error` : undefined}
            aria-label=${this.label ? undefined : this.ariaLabel || undefined}
            .value=${this._inputValue ?? this._displayValue}
            placeholder=${this.placeholder}
            ?disabled=${this.disabled}
            class=${this.error ? "error" : ""}
            @input=${this._onInput}
            @focus=${this._onFocus}
            @blur=${this._onBlur}
            @keydown=${this._onKeyDown}
          />
          ${this._open && filtered.length > 0
            ? html`
                <div
                  id=${this._listboxId}
                  class="dropdown"
                  role="listbox"
                  @mousedown=${(e: MouseEvent) => e.preventDefault()}
                >
                  ${filtered.map(
                    (opt, i) => html`
                      <div
                        id=${`${this._listboxId}-opt-${i}`}
                        class="option"
                        role="option"
                        aria-selected=${i === this._activeIndex}
                        @click=${() => this._selectOption(opt)}
                      >
                        ${this._highlightLabel(opt.label)}
                      </div>
                    `,
                  )}
                </div>
              `
            : null}
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
    "hu-combobox": ScCombobox;
  }
}
