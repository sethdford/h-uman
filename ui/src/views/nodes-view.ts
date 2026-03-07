import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/sc-data-table-v2.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-data-table-v2.js";
import "../components/sc-empty-state.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-sheet.js";
import "../components/sc-skeleton.js";
import "../components/sc-stat-card.js";

interface NodeItem {
  id?: string;
  type?: string;
  status?: string;
  ws_connections?: number;
  uptime_secs?: number;
  hostname?: string;
  version?: string;
}

function formatUptime(secs: number | undefined): string {
  if (secs == null || secs <= 0) return "-";
  const d = Math.floor(secs / 86400);
  const h = Math.floor((secs % 86400) / 3600);
  const m = Math.floor((secs % 3600) / 60);
  const parts: string[] = [];
  if (d > 0) parts.push(`${d}d`);
  if (h > 0) parts.push(`${h}h`);
  if (m > 0 || parts.length === 0) parts.push(`${m}m`);
  return parts.join(" ");
}

@customElement("sc-nodes-view")
export class ScNodesView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-nodes;
      display: block;
      max-width: 75rem;
    }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
    }
    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
    .header-actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-lg);
    }
    .staleness {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .table-section {
      margin-top: var(--sc-space-md);
    }
    .sheet-detail-row {
      display: flex;
      justify-content: space-between;
      padding: var(--sc-space-sm) 0;
      border-bottom: 1px solid var(--sc-border-subtle);
    }
    .sheet-detail-row:last-child {
      border-bottom: none;
    }
    .sheet-detail-label {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .sheet-detail-value {
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      font-family: var(--sc-font-mono);
    }
    .sheet-title {
      font-weight: var(--sc-weight-semibold);
      font-size: var(--sc-text-lg);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-lg);
      font-family: var(--sc-font-mono);
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
    }
  `;

  @state() private nodes: NodeItem[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private _sheetNode: NodeItem | null = null;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const [nodesPayload] = await Promise.all([
        gw.request<{ nodes?: NodeItem[] }>("nodes.list", {}),
      ]);
      this.nodes = nodesPayload?.nodes ?? [];
      this.lastLoadedAt = Date.now();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load";
      this.nodes = [];
    } finally {
      this.loading = false;
    }
  }

  private statusVariant(status: string | undefined): "success" | "warning" | "error" | "neutral" {
    const s = (status ?? "").toLowerCase();
    if (s === "ok" || s === "healthy" || s === "connected" || s === "online") return "success";
    if (s === "degraded" || s === "warning") return "warning";
    return "neutral";
  }

  private get tableRows(): Record<string, unknown>[] {
    return this.nodes.map((n) => ({
      id: n.id ?? "—",
      status: n.status ?? "unknown",
      statusVariant: this.statusVariant(n.status),
      wsCount: n.ws_connections ?? 0,
      uptime: formatUptime(n.uptime_secs),
      _node: n,
    }));
  }

  private readonly columns: DataTableColumnV2[] = [
    { key: "id", label: "Node ID", sortable: true },
    {
      key: "status",
      label: "Status",
      render: (_v, row) => {
        const variant = row.statusVariant as "success" | "warning" | "neutral";
        const label = String(row.status ?? "");
        return html`<sc-badge variant=${variant}>${label}</sc-badge>`;
      },
    },
    {
      key: "wsCount",
      label: "WebSocket Count",
      align: "right",
      sortable: true,
      render: (v) => String(v ?? 0),
    },
    { key: "uptime", label: "Uptime" },
  ];

  private _onRowClick(e: CustomEvent<{ row: Record<string, unknown>; index: number }>): void {
    const node = e.detail.row._node as NodeItem;
    this._sheetNode = node ?? null;
  }

  private _onSheetClose(): void {
    this._sheetNode = null;
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  private _renderSkeleton(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Nodes overview">
        <sc-section-header
          heading="Nodes"
          description="Connected node instances and their status"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
      </div>
      <div class="table-section">
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
      </div>
    `;
  }

  private _renderContent(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Nodes overview">
        <sc-section-header
          heading="Nodes"
          description="Connected node instances and their status"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-stat-card
          .value=${this.nodes.length}
          label="Total Nodes"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.nodes.filter((n) =>
            ["ok", "healthy", "connected", "online"].includes((n.status ?? "").toLowerCase()),
          ).length}
          label="Healthy"
          style="--sc-stagger-delay: 50ms"
        ></sc-stat-card>
      </div>
      <div class="header-actions">
        ${this.lastLoadedAt
          ? html`<span class="staleness">Last updated ${this.stalenessLabel}</span>`
          : nothing}
        <sc-button
          size="sm"
          .loading=${this.loading}
          @click=${() => this.load()}
          aria-label="Refresh nodes"
        >
          Refresh
        </sc-button>
      </div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            .description=${this.error}
          ></sc-empty-state>`
        : nothing}
      <div class="table-section" role="region" aria-label="Nodes table">
        ${this.nodes.length === 0 && !this.loading
          ? html`
              <sc-empty-state
                .icon=${icons.monitor}
                heading="No nodes connected"
                description="Connected devices and gateways will appear here."
              ></sc-empty-state>
            `
          : this.nodes.length > 0
            ? html`
                <sc-data-table-v2
                  .columns=${this.columns}
                  .rows=${this.tableRows}
                  @sc-row-click=${this._onRowClick}
                ></sc-data-table-v2>
              `
            : nothing}
      </div>
      <sc-sheet ?open=${this._sheetNode != null} @close=${this._onSheetClose}>
        ${this._sheetNode
          ? html`
              <div class="sheet-title">${this._sheetNode.id ?? "Node"}</div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Status</span>
                <span class="sheet-detail-value">${this._sheetNode.status ?? "-"}</span>
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Type</span>
                <span class="sheet-detail-value">${this._sheetNode.type ?? "-"}</span>
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">WebSocket connections</span>
                <span class="sheet-detail-value">${this._sheetNode.ws_connections ?? 0}</span>
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Uptime</span>
                <span class="sheet-detail-value">${formatUptime(this._sheetNode.uptime_secs)}</span>
              </div>
              ${this._sheetNode.hostname
                ? html`
                    <div class="sheet-detail-row">
                      <span class="sheet-detail-label">Hostname</span>
                      <span class="sheet-detail-value">${this._sheetNode.hostname}</span>
                    </div>
                  `
                : nothing}
              ${this._sheetNode.version
                ? html`
                    <div class="sheet-detail-row">
                      <span class="sheet-detail-label">Version</span>
                      <span class="sheet-detail-value">${this._sheetNode.version}</span>
                    </div>
                  `
                : nothing}
            `
          : nothing}
      </sc-sheet>
    `;
  }

  override render() {
    if (this.loading && this.nodes.length === 0) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
