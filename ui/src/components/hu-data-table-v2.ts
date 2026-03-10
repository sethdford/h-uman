import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import "./hu-pagination.js";

export interface DataTableColumnV2 {
  key: string;
  label: string;
  align?: "left" | "center" | "right";
  width?: string;
  sortable?: boolean;
  filterable?: boolean;
  render?: (value: unknown, row: Record<string, unknown>) => unknown;
}

@customElement("hu-data-table-v2")
export class ScDataTableV2 extends LitElement {
  @property({ type: Array }) columns: DataTableColumnV2[] = [];
  @property({ type: Array }) rows: Record<string, unknown>[] = [];
  @property({ type: Boolean }) striped = true;
  @property({ type: Boolean }) hoverable = true;
  @property({ type: Boolean }) compact = false;
  @property({ type: Number }) pageSize = 10;
  @property({ type: Boolean }) paginated = true;
  @property({ type: Boolean }) searchable = false;

  @state() private _sortKey: string | null = null;
  @state() private _sortDir: "asc" | "desc" | null = null;
  @state() private _page = 1;
  @state() private _search = "";
  @state() private _focusedRowIndex = -1;

  private get _filteredRows(): Record<string, unknown>[] {
    if (!this._search.trim()) return this.rows;
    const q = this._search.toLowerCase();
    return this.rows.filter((row) =>
      this.columns.some((col) => {
        const v = row[col.key];
        return typeof v === "string" && v.toLowerCase().includes(q);
      }),
    );
  }

  private get _sortedRows(): Record<string, unknown>[] {
    const rows = [...this._filteredRows];
    if (!this._sortKey || !this._sortDir) return rows;
    rows.sort((a, b) => {
      const av = a[this._sortKey!];
      const bv = b[this._sortKey!];
      const cmp =
        av == null && bv == null
          ? 0
          : av == null
            ? 1
            : bv == null
              ? -1
              : String(av).localeCompare(String(bv), undefined, { numeric: true });
      return this._sortDir === "asc" ? cmp : -cmp;
    });
    return rows;
  }

  private get _paginatedRows(): Record<string, unknown>[] {
    if (!this.paginated) return this._sortedRows;
    const start = (this._page - 1) * this.pageSize;
    return this._sortedRows.slice(start, start + this.pageSize);
  }

  private get _totalFiltered(): number {
    return this._filteredRows.length;
  }

  private get _pageCount(): number {
    if (this._totalFiltered <= 0 || this.pageSize <= 0) return 0;
    return Math.ceil(this._totalFiltered / this.pageSize);
  }

  private _cellValue(row: Record<string, unknown>, col: DataTableColumnV2): unknown {
    const v = row[col.key];
    if (col.render) return col.render(v, row);
    return v ?? "";
  }

  private _onSort(col: DataTableColumnV2): void {
    if (!col.sortable) return;
    let dir: "asc" | "desc" | null = "asc";
    if (this._sortKey === col.key) {
      dir = this._sortDir === "asc" ? "desc" : null;
    }
    this._sortKey = dir ? col.key : null;
    this._sortDir = dir;
    this._page = 1;
    this.dispatchEvent(
      new CustomEvent("hu-sort-change", {
        bubbles: true,
        composed: true,
        detail: { key: col.key, direction: dir },
      }),
    );
  }

  private _onRowClick(row: Record<string, unknown>, index: number): void {
    this.dispatchEvent(
      new CustomEvent("hu-row-click", {
        bubbles: true,
        composed: true,
        detail: { row, index },
      }),
    );
  }

  private _onPageChange(e: CustomEvent<{ page: number; pageSize: number }>): void {
    this._page = e.detail.page;
    this.pageSize = e.detail.pageSize;
  }

  private _onSearchInput(e: Event): void {
    this._search = (e.target as HTMLInputElement).value;
    this._page = 1;
  }

  private _onKeydown(e: KeyboardEvent, index: number): void {
    const visibleRows = this._paginatedRows;
    if (e.key === "ArrowDown" && index < visibleRows.length - 1) {
      e.preventDefault();
      this._focusedRowIndex = index + 1;
      (e.target as HTMLElement).parentElement?.nextElementSibling?.querySelector("td")?.focus();
    } else if (e.key === "ArrowUp" && index > 0) {
      e.preventDefault();
      this._focusedRowIndex = index - 1;
      (e.target as HTMLElement).parentElement?.previousElementSibling?.querySelector("td")?.focus();
    } else if (e.key === "Enter") {
      e.preventDefault();
      this._onRowClick(visibleRows[index], index);
    }
  }

  static override styles = css`
    :host {
      display: block;
      overflow-x: auto;
      font-family: var(--hu-font);
    }

    .search-wrap {
      margin-bottom: var(--hu-space-md);
    }

    .search-wrap input {
      width: 100%;
      max-width: 20rem;
      box-sizing: border-box;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .search-wrap input::placeholder {
      color: var(--hu-text-muted);
    }

    .search-wrap input:focus-visible {
      border-color: var(--hu-accent);
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .table-wrap {
      overflow-x: auto;
    }

    table {
      width: 100%;
      border-collapse: collapse;
    }

    thead {
      position: sticky;
      top: 0;
      z-index: 1;
      background: var(--hu-bg-surface);
    }

    th {
      text-align: left;
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      padding: var(--hu-space-sm) var(--hu-space-md);
      border-bottom: 1px solid var(--hu-border-subtle);
      white-space: nowrap;
      background: var(--hu-bg-surface);
    }

    th[data-align="center"] {
      text-align: center;
    }

    th[data-align="right"] {
      text-align: right;
    }

    th.sortable {
      cursor: pointer;
      user-select: none;
    }

    th.sortable:hover {
      color: var(--hu-text);
    }

    .sort-indicator {
      display: inline-flex;
      margin-left: var(--hu-space-xs);
      color: var(--hu-text-muted);
    }

    th[aria-sort="ascending"] .sort-indicator,
    th[aria-sort="descending"] .sort-indicator {
      color: var(--hu-accent);
    }

    td {
      font-size: var(--hu-text-base);
      color: var(--hu-text);
      padding: var(--hu-space-sm) var(--hu-space-md);
      border-bottom: 1px solid var(--hu-border-subtle);
      font-variant-numeric: tabular-nums;
    }

    td[data-align="center"] {
      text-align: center;
    }

    td[data-align="right"] {
      text-align: right;
    }

    tr.striped:nth-child(even) td {
      background: var(--hu-bg-inset);
    }

    tr.striped:nth-child(odd) td {
      background: var(--hu-bg-surface);
    }

    tr.hoverable:hover td {
      background: var(--hu-hover-overlay);
    }

    tr:last-child td {
      border-bottom: none;
    }

    tr[role="row"] {
      cursor: pointer;
    }

    td:focus {
      outline: none;
    }

    tr:focus-within td {
      box-shadow: inset 0 0 0 2px var(--hu-accent);
    }

    .compact th,
    .compact td {
      padding: var(--hu-space-xs) var(--hu-space-sm);
    }

    .compact th {
      font-size: 0.625rem;
    }

    .compact td {
      font-size: var(--hu-text-sm);
    }

    .empty {
      padding: var(--hu-space-xl);
      text-align: center;
      color: var(--hu-text-muted);
      font-size: var(--hu-text-sm);
    }

    .pagination-wrap {
      margin-top: var(--hu-space-md);
    }

    @media (max-width: 640px) /* --hu-breakpoint-md */ {
      .table-wrap table {
        display: none;
      }

      .cards {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-sm);
      }

      .card {
        padding: var(--hu-space-md);
        background: var(--hu-bg-surface);
        border: 1px solid var(--hu-border-subtle);
        border-radius: var(--hu-radius);
      }

      .card-row {
        display: flex;
        justify-content: space-between;
        padding: var(--hu-space-xs) 0;
        border-bottom: 1px solid var(--hu-border-subtle);
      }

      .card-row:last-child {
        border-bottom: none;
      }

      .card-label {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
        text-transform: uppercase;
      }

      .card-value {
        font-size: var(--hu-text-sm);
      }
    }

    @media (min-width: 641px) /* --hu-breakpoint-md */ {
      .cards {
        display: none;
      }
    }
  `;

  override render() {
    const rowClass = `${this.striped ? "striped" : ""} ${this.hoverable ? "hoverable" : ""}`.trim();
    const tableClass = this.compact ? "compact" : "";
    const visibleRows = this._paginatedRows;
    const totalFiltered = this._totalFiltered;
    const isEmpty = this.rows.length === 0;
    const noResults = !isEmpty && totalFiltered === 0;

    return html`
      ${this.searchable
        ? html`
            <div class="search-wrap">
              <input
                type="search"
                placeholder="Search..."
                .value=${this._search}
                @input=${this._onSearchInput}
                aria-label="Search table"
              />
            </div>
          `
        : null}
      <div class="table-wrap">
        ${this.columns.length > 0
          ? html`
              <table
                class=${tableClass}
                role="grid"
                aria-rowcount=${this._sortedRows.length}
                aria-colcount=${this.columns.length}
              >
                <thead>
                  <tr role="row">
                    ${this.columns.map(
                      (col) => html`
                        <th
                          role="columnheader"
                          scope="col"
                          data-align=${col.align || "left"}
                          style=${col.width ? `width: ${col.width}` : ""}
                          class=${col.sortable ? "sortable" : ""}
                          aria-sort=${col.sortable && this._sortKey === col.key
                            ? this._sortDir === "asc"
                              ? "ascending"
                              : "descending"
                            : undefined}
                          @click=${() => this._onSort(col)}
                        >
                          ${col.label}
                          ${col.sortable
                            ? html`
                                <span class="sort-indicator" aria-hidden="true">
                                  ${this._sortKey === col.key && this._sortDir === "asc"
                                    ? "▲"
                                    : this._sortKey === col.key && this._sortDir === "desc"
                                      ? "▼"
                                      : ""}
                                </span>
                              `
                            : null}
                        </th>
                      `,
                    )}
                  </tr>
                </thead>
                <tbody>
                  ${visibleRows.map(
                    (row, i) => html`
                      <tr
                        role="row"
                        class=${rowClass}
                        tabindex="-1"
                        @click=${() => this._onRowClick(row, i)}
                        @keydown=${(e: KeyboardEvent) => this._onKeydown(e, i)}
                      >
                        ${this.columns.map(
                          (col) => html`
                            <td
                              role="gridcell"
                              data-align=${col.align || "left"}
                              tabindex=${i === this._focusedRowIndex ? 0 : -1}
                            >
                              ${this._cellValue(row, col)}
                            </td>
                          `,
                        )}
                      </tr>
                    `,
                  )}
                </tbody>
              </table>
            `
          : null}
        <div class="cards" role="list">
          ${visibleRows.map(
            (row, i) => html`
              <div
                class="card"
                role="listitem"
                tabindex="0"
                @click=${() => this._onRowClick(row, i)}
                @keydown=${(e: KeyboardEvent) => {
                  if (e.key === "Enter") this._onRowClick(row, i);
                }}
              >
                ${this.columns.map(
                  (col) => html`
                    <div class="card-row">
                      <span class="card-label">${col.label}</span>
                      <span class="card-value">${this._cellValue(row, col)}</span>
                    </div>
                  `,
                )}
              </div>
            `,
          )}
        </div>
      </div>
      ${isEmpty
        ? html`<div class="empty">No data</div>`
        : noResults
          ? html`<div class="empty">No results</div>`
          : null}
      ${this.paginated && totalFiltered > 0
        ? html`
            <div class="pagination-wrap">
              <hu-pagination
                .total=${totalFiltered}
                .page=${this._page}
                .pageSize=${this.pageSize}
                @hu-page-change=${this._onPageChange}
              ></hu-pagination>
            </div>
          `
        : null}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-data-table-v2": ScDataTableV2;
  }
}
