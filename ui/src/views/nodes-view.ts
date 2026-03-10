import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/hu-data-table-v2.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-data-table-v2.js";
import "../components/hu-empty-state.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-sheet.js";
import "../components/hu-skeleton.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";

interface NodeItem {
  id?: string;
  type?: string;
  status?: string;
  ws_connections?: number;
  uptime_secs?: number;
  hostname?: string;
  version?: string;
}

function friendlyError(e: unknown): string {
  const msg = e instanceof Error ? e.message : String(e);
  if (msg.includes("timeout")) return "Request timed out. Please try again.";
  if (msg.includes("WebSocket")) return "Connection lost. Reconnecting...";
  if (msg.includes("404")) return "Resource not found.";
  if (msg.includes("401") || msg.includes("unauthorized"))
    return "Authentication failed. Please check your credentials.";
  if (msg.includes("403") || msg.includes("forbidden")) return "Access denied.";
  if (msg.includes("network")) return "Network error. Please check your connection.";
  return "Something went wrong. Please try again.";
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

@customElement("hu-nodes-view")
export class ScNodesView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-nodes;
      display: block;
      max-width: 75rem;
    }
    .header-actions {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
      margin-bottom: var(--hu-space-lg);
    }
    .staleness {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }
    .table-section {
      margin-top: var(--hu-space-md);
    }
    .sheet-detail-row {
      display: flex;
      justify-content: space-between;
      padding: var(--hu-space-sm) 0;
      border-bottom: 1px solid var(--hu-border-subtle);
    }
    .sheet-detail-row:last-child {
      border-bottom: none;
    }
    .sheet-detail-label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }
    .sheet-detail-value {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-family: var(--hu-font-mono);
    }
    .sheet-title {
      font-weight: var(--hu-weight-semibold);
      font-size: var(--hu-text-lg);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-lg);
      font-family: var(--hu-font-mono);
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
      this.error = friendlyError(e);
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
        return html`<hu-badge variant=${variant}>${label}</hu-badge>`;
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
      <hu-page-hero role="region" aria-label="Nodes overview">
        <hu-section-header
          heading="Nodes"
          description="Connected node instances and their status"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <div class="table-section">
        <hu-skeleton variant="card" height="200px"></hu-skeleton>
      </div>
    `;
  }

  private _renderContent(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Nodes overview">
        <hu-section-header
          heading="Nodes"
          description="Connected node instances and their status"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-stat-card
          .value=${this.nodes.length}
          label="Total Nodes"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.nodes.filter((n) =>
            ["ok", "healthy", "connected", "online"].includes((n.status ?? "").toLowerCase()),
          ).length}
          label="Healthy"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
      </hu-stats-row>
      <div class="header-actions">
        ${this.lastLoadedAt
          ? html`<span class="staleness">Last updated ${this.stalenessLabel}</span>`
          : nothing}
        <hu-button
          size="sm"
          .loading=${this.loading}
          @click=${() => this.load()}
          aria-label="Refresh nodes"
        >
          Refresh
        </hu-button>
      </div>
      ${this.error
        ? html`<hu-empty-state
            .icon=${icons.warning}
            heading="Error"
            .description=${this.error}
          ></hu-empty-state>`
        : nothing}
      <div class="table-section" role="region" aria-label="Nodes table">
        ${this.nodes.length === 0 && !this.loading
          ? html`
              <hu-empty-state
                .icon=${icons.monitor}
                heading="No nodes connected"
                description="Connected devices and gateways will appear here."
              ></hu-empty-state>
            `
          : this.nodes.length > 0
            ? html`
                <hu-data-table-v2
                  .columns=${this.columns}
                  .rows=${this.tableRows}
                  @hu-row-click=${this._onRowClick}
                ></hu-data-table-v2>
              `
            : nothing}
      </div>
      <hu-sheet ?open=${this._sheetNode != null} @close=${this._onSheetClose}>
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
      </hu-sheet>
    `;
  }

  override render() {
    if (this.loading && this.nodes.length === 0) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
