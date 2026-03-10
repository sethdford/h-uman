import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-chat-search")
export class ScChatSearch extends LitElement {
  @property({ type: Boolean }) open = false;

  @property({ type: String }) query = "";

  @property({ type: Number }) matchCount = 0;

  @property({ type: Number }) currentMatch = 0;

  @state() private _inputValue = "";

  @query("#search-input") private _inputEl!: HTMLInputElement;

  static override styles = css`
    :host {
      display: block;
    }

    .bar {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: color-mix(
        in srgb,
        var(--hu-bg-surface) var(--hu-glass-standard-bg-opacity, 6%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      border: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 8%),
          transparent
        );
      border-radius: var(--hu-radius);
      box-shadow: var(--hu-shadow-sm);
      animation: hu-slide-down var(--hu-duration-normal) var(--hu-ease-out) both;
    }

    .input-wrap {
      flex: 1;
      position: relative;
      display: flex;
      align-items: center;
    }

    .input-wrap input {
      flex: 1;
      min-width: 0;
      padding: var(--hu-space-xs) var(--hu-space-md);
      padding-right: calc(var(--hu-space-md) + 1.25rem);
      background: var(--hu-bg);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      line-height: 1.5;
      transition: border-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .input-wrap input:focus {
      outline: none;
      border-color: var(--hu-accent);
      box-shadow: 0 0 0 3px var(--hu-accent-subtle);
    }

    .input-wrap input::placeholder {
      color: var(--hu-text-muted);
    }

    .clear-btn {
      position: absolute;
      right: var(--hu-space-xs);
      top: 50%;
      transform: translateY(-50%);
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      border: none;
      background: none;
      padding: var(--hu-space-2xs);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .clear-btn:hover {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }

    .clear-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .clear-btn svg {
      width: 1rem;
      height: 1rem;
    }

    .nav-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      border: none;
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      padding: var(--hu-space-xs);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .nav-btn:hover:not(:disabled) {
      color: var(--hu-text);
      background: var(--hu-bg-surface);
      border-color: var(--hu-accent);
    }

    .nav-btn:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .nav-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .nav-btn svg {
      width: 1rem;
      height: 1rem;
    }

    .match-count {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      font-variant-numeric: tabular-nums;
      min-width: 4ch;
    }

    .close-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      border: none;
      background: none;
      padding: var(--hu-space-2xs);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .close-btn:hover {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }

    .close-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .close-btn svg {
      width: 1rem;
      height: 1rem;
    }

    @media (prefers-reduced-motion: reduce) {
      .bar {
        animation: none;
      }
    }

    @supports not (backdrop-filter: blur(1px)) {
      .bar {
        background: var(--hu-bg-surface);
        border: 1px solid var(--hu-border);
      }
    }
  `;

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("open") && this.open) {
      this._inputValue = this.query;
      this.updateComplete.then(() => {
        this._inputEl?.focus();
      });
    }
    if (changed.has("query")) {
      this._inputValue = this.query;
    }
  }

  private _handleInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this._inputValue = target.value ?? "";
    this.dispatchEvent(
      new CustomEvent("hu-search-change", {
        detail: { query: this._inputValue },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _handleKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this.dispatchEvent(new CustomEvent("hu-search-close", { bubbles: true, composed: true }));
      return;
    }
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this.dispatchEvent(new CustomEvent("hu-search-next", { bubbles: true, composed: true }));
      return;
    }
    if (e.key === "Enter" && e.shiftKey) {
      e.preventDefault();
      this.dispatchEvent(new CustomEvent("hu-search-prev", { bubbles: true, composed: true }));
    }
  }

  private _handleClear(): void {
    this._inputValue = "";
    this.dispatchEvent(
      new CustomEvent("hu-search-change", {
        detail: { query: "" },
        bubbles: true,
        composed: true,
      }),
    );
    this._inputEl?.focus();
  }

  private _handleClose(): void {
    this.dispatchEvent(new CustomEvent("hu-search-close", { bubbles: true, composed: true }));
  }

  private _handleNext(): void {
    this.dispatchEvent(new CustomEvent("hu-search-next", { bubbles: true, composed: true }));
  }

  private _handlePrev(): void {
    this.dispatchEvent(new CustomEvent("hu-search-prev", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open) return nothing;

    const hasQuery = this._inputValue.length > 0;
    const hasMatches = this.matchCount > 0;
    const matchText =
      hasQuery && hasMatches
        ? `${this.currentMatch} of ${this.matchCount}`
        : hasQuery && !hasMatches
          ? "No matches"
          : "";

    return html`
      <div class="bar" role="search" aria-label="Search in conversation">
        <div class="input-wrap">
          <input
            id="search-input"
            type="search"
            .value=${this._inputValue}
            placeholder="Search..."
            aria-label="Search query"
            @input=${this._handleInput}
            @keydown=${this._handleKeyDown}
          />
          ${hasQuery
            ? html`
                <button
                  class="clear-btn"
                  type="button"
                  aria-label="Clear search"
                  @click=${this._handleClear}
                >
                  ${icons.x}
                </button>
              `
            : nothing}
        </div>
        <button
          class="nav-btn"
          type="button"
          aria-label="Previous match"
          ?disabled=${!hasMatches}
          @click=${this._handlePrev}
        >
          ${icons["caret-up"]}
        </button>
        <button
          class="nav-btn"
          type="button"
          aria-label="Next match"
          ?disabled=${!hasMatches}
          @click=${this._handleNext}
        >
          ${icons["caret-down"]}
        </button>
        ${matchText ? html`<span class="match-count">${matchText}</span>` : nothing}
        <button
          class="close-btn"
          type="button"
          aria-label="Close search"
          @click=${this._handleClose}
        >
          ${icons.x}
        </button>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-chat-search": ScChatSearch;
  }
}
