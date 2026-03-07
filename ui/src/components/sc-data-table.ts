import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export interface DataTableColumn {
  key: string;
  label: string;
  align?: "left" | "center" | "right";
  width?: string;
}

/**
 * @deprecated Use sc-data-table-v2 instead. Still used in: components.test.ts, catalog.html
 */
@customElement("sc-data-table")
export class ScDataTable extends LitElement {
  @property({ type: Array }) columns: DataTableColumn[] = [];
  @property({ type: Array }) rows: Record<string, unknown>[] = [];
  @property({ type: Boolean }) striped = true;
  @property({ type: Boolean }) hoverable = true;
  @property({ type: Boolean }) compact = false;

  static override styles = css`
    :host {
      display: block;
      overflow-x: auto;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-family: var(--sc-font);
    }

    th {
      text-align: left;
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-bottom: 1px solid var(--sc-border-subtle);
      white-space: nowrap;
    }

    th[data-align="center"] {
      text-align: center;
    }

    th[data-align="right"] {
      text-align: right;
    }

    td {
      font-size: var(--sc-text-base);
      color: var(--sc-text);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-bottom: 1px solid var(--sc-border-subtle);
      font-variant-numeric: tabular-nums;
    }

    td[data-align="center"] {
      text-align: center;
    }

    td[data-align="right"] {
      text-align: right;
    }

    tr.striped:nth-child(even) td {
      background: var(--sc-bg-inset);
    }

    tr.striped:nth-child(odd) td {
      background: var(--sc-bg-surface);
    }

    tr.hoverable:hover td {
      background: var(--sc-bg-elevated);
    }

    tr:last-child td {
      border-bottom: none;
    }

    .compact th,
    .compact td {
      padding: var(--sc-space-xs) var(--sc-space-sm);
    }

    .compact th {
      font-size: 0.625rem;
    }

    .compact td {
      font-size: var(--sc-text-sm);
    }
  `;

  private _cellValue(row: Record<string, unknown>, key: string): unknown {
    return row[key];
  }

  override render() {
    if (this.columns.length === 0) return html``;

    const tableClass = this.compact ? "compact" : "";
    const rowClass = `${this.striped ? "striped" : ""} ${this.hoverable ? "hoverable" : ""}`.trim();

    return html`
      <table class=${tableClass}>
        <thead>
          <tr>
            ${this.columns.map(
              (col) => html`
                <th
                  scope="col"
                  data-align=${col.align || "left"}
                  style=${col.width ? `width: ${col.width}` : ""}
                >
                  ${col.label}
                </th>
              `,
            )}
          </tr>
        </thead>
        <tbody>
          ${this.rows.map(
            (row) => html`
              <tr class=${rowClass}>
                ${this.columns.map(
                  (col) => html`
                    <td data-align=${col.align || "left"}>
                      ${this._cellValue(row, col.key) ?? ""}
                    </td>
                  `,
                )}
              </tr>
            `,
          )}
        </tbody>
      </table>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-data-table": ScDataTable;
  }
}
