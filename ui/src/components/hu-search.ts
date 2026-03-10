import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

type SearchSize = "sm" | "md" | "lg";

@customElement("hu-search")
export class ScSearch extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "Search...";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) size: SearchSize = "md";

  @state() private _inputId = `hu-search-${Math.random().toString(36).slice(2, 11)}`;

  private _debounceTimer: ReturnType<typeof setTimeout> | null = null;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      position: relative;
      display: flex;
      align-items: center;
    }

    .icon-start {
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

    .icon-start svg,
    .icon-end svg {
      width: 1.125rem;
      height: 1.125rem;
    }

    .icon-end {
      position: absolute;
      right: var(--hu-space-sm);
      top: 50%;
      transform: translateY(-50%);
      color: var(--hu-text-muted);
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      border: none;
      background: none;
      padding: var(--hu-space-2xs);
      border-radius: var(--hu-radius-sm);
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .icon-end:hover:not(:disabled) {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }

    .icon-end:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: 2px;
    }

    .icon-end:disabled {
      display: none;
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

    input:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    input.size-sm {
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      padding-left: calc(var(--hu-space-sm) + 1.125rem + var(--hu-space-xs));
      padding-right: calc(var(--hu-space-sm) + 1.125rem + var(--hu-space-xs));
      min-height: calc(var(--hu-input-min-height) - var(--hu-space-sm));
    }

    input.size-md {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      padding-left: calc(var(--hu-space-md) + 1.125rem + var(--hu-space-xs));
      padding-right: calc(var(--hu-space-md) + 1.125rem + var(--hu-space-xs));
      min-height: var(--hu-input-min-height);
    }

    input.size-lg {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-md);
      padding-left: calc(var(--hu-space-md) + 1.125rem + var(--hu-space-xs));
      padding-right: calc(var(--hu-space-md) + 1.125rem + var(--hu-space-xs));
      min-height: calc(var(--hu-input-min-height) + var(--hu-space-sm));
    }
  `;

  private _emitSearch(): void {
    this.dispatchEvent(
      new CustomEvent("hu-search", {
        bubbles: true,
        composed: true,
        detail: { value: this.value },
      }),
    );
  }

  private _onInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this.value = target.value;

    if (this._debounceTimer) clearTimeout(this._debounceTimer);
    this._debounceTimer = setTimeout(() => {
      this._debounceTimer = null;
      this._emitSearch();
    }, 200);
  }

  private _onClear(): void {
    if (this.disabled) return;
    this.value = "";
    if (this._debounceTimer) {
      clearTimeout(this._debounceTimer);
      this._debounceTimer = null;
    }
    this.dispatchEvent(
      new CustomEvent("hu-clear", {
        bubbles: true,
        composed: true,
      }),
    );
    this._emitSearch();
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this._onClear();
    }
  }

  override disconnectedCallback(): void {
    if (this._debounceTimer) clearTimeout(this._debounceTimer);
    super.disconnectedCallback();
  }

  override render() {
    const showClear = this.value.length > 0 && !this.disabled;
    return html`
      <div class="wrapper" role="search" aria-label="Search">
        <span class="icon-start" aria-hidden="true">${icons.magnifyingGlass}</span>
        <input
          id=${this._inputId}
          class="size-${this.size}"
          type="search"
          .value=${this.value}
          placeholder=${this.placeholder}
          ?disabled=${this.disabled}
          autocomplete="off"
          aria-label="Search"
          @input=${this._onInput}
          @keydown=${this._onKeyDown}
        />
        ${showClear
          ? html`<button
              type="button"
              class="icon-end"
              aria-label="Clear search"
              @click=${this._onClear}
            >
              ${icons.xCircle}
            </button>`
          : null}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-search": ScSearch;
  }
}
