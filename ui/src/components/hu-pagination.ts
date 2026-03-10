import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-pagination")
export class ScPagination extends LitElement {
  @property({ type: Number }) total = 0;
  @property({ type: Number }) page = 1;
  @property({ type: Number }) pageSize = 10;
  @property({ type: Array }) pageSizes: number[] = [10, 25, 50];

  static override get properties() {
    return {
      total: { type: Number },
      page: { type: Number },
      pageSize: { type: Number },
      pageSizes: { type: Array },
    };
  }

  get pageCount(): number {
    if (this.total <= 0 || this.pageSize <= 0) return 0;
    return Math.ceil(this.total / this.pageSize);
  }

  private get _start(): number {
    return this.total === 0 ? 0 : (this.page - 1) * this.pageSize + 1;
  }

  private get _end(): number {
    return Math.min(this.page * this.pageSize, this.total);
  }

  private get _pageNumbers(): (number | "ellipsis")[] {
    const count = this.pageCount;
    if (count <= 7) {
      return Array.from({ length: count }, (_, i) => i + 1);
    }
    const p = this.page;
    if (p <= 4) {
      return [1, 2, 3, 4, 5, "ellipsis", count];
    }
    if (p >= count - 3) {
      return [1, "ellipsis", count - 4, count - 3, count - 2, count - 1, count];
    }
    return [1, "ellipsis", p - 1, p, p + 1, "ellipsis", count];
  }

  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font);
    }

    .pagination {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--hu-space-sm);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .label {
      margin-right: var(--hu-space-sm);
    }

    .nav {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-width: 2rem;
      min-height: 2rem;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
    }

    @media (prefers-reduced-motion: reduce) {
      button {
        transition: none;
      }
    }

    button:hover:not(:disabled) {
      background: var(--hu-hover-overlay);
      border-color: var(--hu-border);
    }

    button:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    button:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    button.active {
      background: var(--hu-accent);
      border-color: var(--hu-accent);
      color: var(--hu-on-accent);
    }

    button.active:hover:not(:disabled) {
      background: var(--hu-accent-hover);
      border-color: var(--hu-accent-hover);
    }

    .ellipsis {
      padding: 0 var(--hu-space-xs);
      color: var(--hu-text-muted);
    }

    .page-size {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .page-size label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .select-wrapper {
      position: relative;
      display: inline-flex;
      align-items: center;
    }
    .select-wrapper .select-arrow {
      position: absolute;
      right: var(--hu-space-sm);
      pointer-events: none;
      color: var(--hu-text-muted);
    }
    .select-wrapper .select-arrow svg {
      width: 0.75rem;
      height: 0.75rem;
    }
    select {
      width: auto;
      min-width: 4rem;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      padding-right: var(--hu-space-xl);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      cursor: pointer;
      appearance: none;
      -webkit-appearance: none;
      -moz-appearance: none;
    }

    select:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .icon {
      display: inline-flex;
      width: 1rem;
      height: 1rem;
    }

    .icon svg {
      width: 1rem;
      height: 1rem;
    }
  `;

  private _goToPage(p: number): void {
    if (p < 1 || p > this.pageCount) return;
    this.dispatchEvent(
      new CustomEvent("hu-page-change", {
        bubbles: true,
        composed: true,
        detail: { page: p, pageSize: this.pageSize },
      }),
    );
  }

  private _onPageSizeChange(e: Event): void {
    const target = e.target as HTMLSelectElement;
    const size = parseInt(target.value, 10);
    this.dispatchEvent(
      new CustomEvent("hu-page-change", {
        bubbles: true,
        composed: true,
        detail: { page: 1, pageSize: size },
      }),
    );
  }

  override render() {
    const count = this.pageCount;
    if (count === 0 && this.total === 0) {
      return html`<div class="pagination"><span class="label">Showing 0 of 0</span></div>`;
    }

    return html`
      <div class="pagination" role="navigation" aria-label="Pagination">
        <span class="label">Showing ${this._start}–${this._end} of ${this.total}</span>
        <div class="nav">
          <button
            type="button"
            aria-label="Previous page"
            ?disabled=${this.page <= 1}
            @click=${() => this._goToPage(this.page - 1)}
          >
            <span class="icon" aria-hidden="true">${icons["caret-left"]}</span>
          </button>
          ${this._pageNumbers.map((n) =>
            n === "ellipsis"
              ? html`<span class="ellipsis" aria-hidden="true">…</span>`
              : html`
                  <button
                    type="button"
                    class=${this.page === n ? "active" : ""}
                    aria-label=${`Page ${n}`}
                    aria-current=${this.page === n ? "page" : undefined}
                    @click=${() => this._goToPage(n)}
                  >
                    ${n}
                  </button>
                `,
          )}
          <button
            type="button"
            aria-label="Next page"
            ?disabled=${this.page >= count}
            @click=${() => this._goToPage(this.page + 1)}
          >
            <span class="icon" aria-hidden="true">${icons["caret-right"]}</span>
          </button>
        </div>
        <div class="page-size">
          <label for="page-size-select">Per page</label>
          <div class="select-wrapper">
            <select
              id="page-size-select"
              aria-label="Items per page"
              .value=${String(this.pageSize)}
              @change=${this._onPageSizeChange}
            >
              ${this.pageSizes.map((s) => html`<option value=${s}>${s}</option>`)}
            </select>
            <span class="select-arrow" aria-hidden="true">${icons["caret-down"]}</span>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-pagination": ScPagination;
  }
}
