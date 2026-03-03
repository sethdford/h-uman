import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

type SearchSize = "sm" | "md" | "lg";

@customElement("sc-search")
export class ScSearch extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: String }) placeholder = "Search...";
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) size: SearchSize = "md";

  @state() private _inputId = `sc-search-${Math.random().toString(36).slice(2, 11)}`;

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
      left: var(--sc-space-sm);
      top: 50%;
      transform: translateY(-50%);
      color: var(--sc-text-muted);
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
      right: var(--sc-space-sm);
      top: 50%;
      transform: translateY(-50%);
      color: var(--sc-text-muted);
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      border: none;
      background: none;
      padding: var(--sc-space-2xs);
      border-radius: var(--sc-radius-sm);
      transition:
        color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .icon-end:hover:not(:disabled) {
      color: var(--sc-text);
      background: var(--sc-bg-elevated);
    }

    .icon-end:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: 2px;
    }

    .icon-end:disabled {
      display: none;
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

    input:focus-visible {
      border-color: var(--sc-accent);
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    input:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    input.size-sm {
      font-size: var(--sc-text-sm);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      padding-left: calc(var(--sc-space-sm) + 1.125rem + var(--sc-space-xs));
      padding-right: calc(var(--sc-space-sm) + 1.125rem + var(--sc-space-xs));
      min-height: calc(var(--sc-input-min-height) - var(--sc-space-sm));
    }

    input.size-md {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      padding-left: calc(var(--sc-space-md) + 1.125rem + var(--sc-space-xs));
      padding-right: calc(var(--sc-space-md) + 1.125rem + var(--sc-space-xs));
      min-height: var(--sc-input-min-height);
    }

    input.size-lg {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-md);
      padding-left: calc(var(--sc-space-md) + 1.125rem + var(--sc-space-xs));
      padding-right: calc(var(--sc-space-md) + 1.125rem + var(--sc-space-xs));
      min-height: calc(var(--sc-input-min-height) + var(--sc-space-sm));
    }
  `;

  private _emitSearch(): void {
    this.dispatchEvent(
      new CustomEvent("sc-search", {
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
      new CustomEvent("sc-clear", {
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
    "sc-search": ScSearch;
  }
}
