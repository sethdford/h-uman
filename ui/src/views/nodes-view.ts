import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";
import "../components/sc-input.js";
import "../components/sc-stat-card.js";
import "../components/sc-sheet.js";

interface NodeItem {
  id: string;
  type?: string;
  status?: string;
  hostname?: string;
  version?: string;
  uptime_secs?: number;
  ws_connections?: number;
  cpu_percent?: number;
  memory_mb?: number;
}

type NodeStatus = "online" | "degraded" | "offline" | "unknown";

function normalizeStatus(raw?: string): NodeStatus {
  const s = (raw ?? "").toLowerCase();
  if (s === "ok" || s === "healthy" || s === "connected" || s === "online") return "online";
  if (s === "degraded" || s === "warning") return "degraded";
  if (s === "offline" || s === "error" || s === "unreachable") return "offline";
  return "unknown";
}

function formatUptime(secs?: number): string {
  if (!secs || secs <= 0) return "\u2014";
  const d = Math.floor(secs / 86400);
  const h = Math.floor((secs % 86400) / 3600);
  const m = Math.floor((secs % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

const STATUS_VARIANT: Record<NodeStatus, string> = {
  online: "success",
  degraded: "warning",
  offline: "error",
  unknown: "neutral",
};

@customElement("sc-nodes-view")
export class ScNodesView extends GatewayAwareLitElement {
  override autoRefreshInterval = 15_000;

  static override styles = css`
    :host { display: block; color: var(--sc-text); }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    .toolbar {
      display: flex; align-items: center; justify-content: space-between;
      flex-wrap: wrap; gap: var(--sc-space-sm); margin-bottom: var(--sc-space-lg);
    }
    .toolbar-left {
      display: flex; align-items: center; gap: var(--sc-space-sm);
      flex: 1; min-width: 200px; max-width: 360px;
    }
    .toolbar-right { display: flex; align-items: center; gap: var(--sc-space-sm); }
    .staleness { font-size: var(--sc-text-xs); color: var(--sc-text-muted); }
    .nodes-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
      gap: var(--sc-space-lg);
    }
    .node-card {
      cursor: pointer;
      transition: box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .node-card:hover { box-shadow: var(--sc-shadow-md); }
    .node-card:focus-visible { outline: 2px solid var(--sc-accent); outline-offset: 2px; }
    .node-card-header {
      display: flex; align-items: center; gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-sm);
    }
    .status-dot {
      width: 10px; height: 10px; border-radius: var(--sc-radius-full); flex-shrink: 0;
    }
    .status-dot.online { background: var(--sc-success); box-shadow: 0 0 6px var(--sc-success); }
    .status-dot.degraded { background: var(--sc-warning); box-shadow: 0 0 6px var(--sc-warning); }
    .status-dot.offline { background: var(--sc-error); }
    .status-dot.unknown { background: var(--sc-text-faint); }
    @keyframes sc-pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    .status-dot.online { animation: sc-pulse 2s var(--sc-ease-in-out) infinite; }
    .node-name {
      font-family: var(--sc-font-mono); font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold); color: var(--sc-text);
    }
    .node-meta {
      display: flex; align-items: center; gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-md);
    }
    .node-type-badge {
      display: inline-block; padding: 2px var(--sc-space-xs);
      font-size: var(--sc-text-2xs, 10px); font-weight: var(--sc-weight-medium);
      text-transform: uppercase; letter-spacing: 0.05em;
      background: var(--sc-bg-inset); color: var(--sc-text-muted);
      border-radius: var(--sc-radius-sm);
    }
    .node-hostname {
      font-size: var(--sc-text-xs); color: var(--sc-text-muted);
      font-family: var(--sc-font-mono);
    }
    .node-stats {
      display: grid; grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-xs) var(--sc-space-md); font-size: var(--sc-text-sm);
    }
    .node-stat-label { color: var(--sc-text-muted); }
    .node-stat-value {
      color: var(--sc-text); font-weight: var(--sc-weight-medium);
      text-align: right; font-family: var(--sc-font-mono);
    }
    .detail-header {
      display: flex; align-items: center; gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    .detail-status-dot { width: 14px; height: 14px; border-radius: var(--sc-radius-full); }
    .detail-status-dot.online { background: var(--sc-success); box-shadow: 0 0 8px var(--sc-success); }
    .detail-status-dot.degraded { background: var(--sc-warning); box-shadow: 0 0 8px var(--sc-warning); }
    .detail-status-dot.offline { background: var(--sc-error); }
    .detail-status-dot.unknown { background: var(--sc-text-faint); }
    .detail-name {
      font-family: var(--sc-font-mono); font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-bold); color: var(--sc-text);
    }
    .detail-grid {
      display: grid; grid-template-columns: auto 1fr;
      gap: var(--sc-space-sm) var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl); font-size: var(--sc-text-sm);
    }
    .detail-label { color: var(--sc-text-muted); font-weight: var(--sc-weight-medium); }
    .detail-value { color: var(--sc-text); font-family: var(--sc-font-mono); }
    .detail-actions {
      display: flex; gap: var(--sc-space-sm); padding-top: var(--sc-space-lg);
      border-top: 1px solid var(--sc-border-subtle);
    }
    @media (max-width: 768px) {
      .nodes-grid { grid-template-columns: 1fr 1fr; }
      .stats-row { grid-template-columns: repeat(2, 1fr); }
    }
    @media (max-width: 480px) { .nodes-grid { grid-template-columns: 1fr; } }
    @media (prefers-reduced-motion: reduce) {
      .status-dot.online { animation: none; }
      .node-card { transition: none; }
    }
  `;

  @state() private nodes: NodeItem[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private search = "";
  @state() private selectedNode: NodeItem | null = null;
  @state() private actionLoading = false;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const payload = await gw.request<{ nodes?: NodeItem[] }>("nodes.list", {});
      this.nodes = payload?.nodes ?? [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load nodes";
      this.nodes = [];
    } finally {
      this.loading = false;
    }
  }

  private get filteredNodes(): NodeItem[] {
    if (!this.search) return this.nodes;
    const q = this.search.toLowerCase();
    return this.nodes.filter(
      (n) =>
        n.id.toLowerCase().includes(q) ||
        (n.hostname ?? "").toLowerCase().includes(q) ||
        (n.type ?? "").toLowerCase().includes(q) ||
        normalizeStatus(n.status).includes(q),
    );
  }

  private get statusCounts() {
    const counts = { total: this.nodes.length, online: 0, degraded: 0, offline: 0 };
    for (const n of this.nodes) {
      const s = normalizeStatus(n.status);
      if (s === "online") counts.online++;
      else if (s === "degraded") counts.degraded++;
      else counts.offline++;
    }
    return counts;
  }

  private async _nodeAction(nodeId: string, action: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.actionLoading = true;
    try {
      await gw.request("nodes.action", { node_id: nodeId, action });
      ScToast.show({
        message: `${action} sent to ${nodeId}`,
        variant: "success",
        duration: 3000,
      });
      await this.load();
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : `Failed to ${action}`,
        variant: "error",
      });
    } finally {
      this.actionLoading = false;
    }
  }

  private _renderStats(): TemplateResult {
    const c = this.statusCounts;
    return html`
      <div class="stats-row">
        <sc-stat-card label="Total Nodes" .value=${c.total}></sc-stat-card>
        <sc-stat-card label="Online" .value=${c.online} accent="primary"></sc-stat-card>
        <sc-stat-card label="Degraded" .value=${c.degraded} accent="secondary"></sc-stat-card>
        <sc-stat-card label="Offline" .value=${c.offline} accent="error"></sc-stat-card>
      </div>
    `;
  }

  private _renderToolbar(): TemplateResult {
    return html`
      <div class="toolbar">
        <div class="toolbar-left">
          <sc-input
            placeholder="Search nodes\u2026"
            .value=${this.search}
            @sc-input=${(e: CustomEvent<{ value: string }>) => { this.search = e.detail.value; }}
            aria-label="Search nodes"
          ></sc-input>
        </div>
        <div class="toolbar-right">
          ${this.lastLoadedAt ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>` : nothing}
          <sc-button size="sm" .loading=${this.loading} @click=${() => this.load()} aria-label="Refresh nodes">Refresh</sc-button>
        </div>
      </div>
    `;
  }

  private _renderNodeCard(node: NodeItem): TemplateResult {
    const status = normalizeStatus(node.status);
    return html`
      <sc-card
        class="node-card" tabindex="0" role="button"
        aria-label="View details for ${node.id}"
        @click=${() => { this.selectedNode = node; }}
        @keydown=${(e: KeyboardEvent) => {
          if (e.key === "Enter" || e.key === " ") { e.preventDefault(); this.selectedNode = node; }
        }}
      >
        <div class="node-card-header">
          <span class="status-dot ${status}" aria-label="${status}" role="img"></span>
          <span class="node-name">${node.id}</span>
          <sc-badge variant=${STATUS_VARIANT[status]}>${status}</sc-badge>
        </div>
        <div class="node-meta">
          ${node.type ? html`<span class="node-type-badge">${node.type}</span>` : nothing}
          ${node.hostname ? html`<span class="node-hostname">${node.hostname}</span>` : nothing}
        </div>
        <div class="node-stats">
          <span class="node-stat-label">Uptime</span>
          <span class="node-stat-value">${formatUptime(node.uptime_secs)}</span>
          <span class="node-stat-label">Connections</span>
          <span class="node-stat-value">${node.ws_connections ?? 0}</span>
          ${node.version ? html`
            <span class="node-stat-label">Version</span>
            <span class="node-stat-value">${node.version}</span>
          ` : nothing}
          ${node.cpu_percent != null ? html`
            <span class="node-stat-label">CPU</span>
            <span class="node-stat-value">${node.cpu_percent}%</span>
          ` : nothing}
        </div>
      </sc-card>
    `;
  }

  private _renderGrid(): TemplateResult {
    if (this.loading && this.nodes.length === 0) {
      return html`
        <div class="nodes-grid">
          <sc-skeleton variant="card" height="180px"></sc-skeleton>
          <sc-skeleton variant="card" height="180px"></sc-skeleton>
          <sc-skeleton variant="card" height="180px"></sc-skeleton>
        </div>
      `;
    }
    const filtered = this.filteredNodes;
    if (this.nodes.length === 0) {
      return html`
        <sc-empty-state
          .icon=${icons.monitor}
          heading="No nodes connected"
          description="Connected devices and gateways will appear here when configured."
        ></sc-empty-state>
      `;
    }
    if (filtered.length === 0) {
      return html`
        <sc-empty-state
          .icon=${icons.magnifyingGlass}
          heading="No matching nodes"
          description="Try a different search term."
        ></sc-empty-state>
      `;
    }
    return html`<div class="nodes-grid">${filtered.map((n) => this._renderNodeCard(n))}</div>`;
  }

  private _renderDetailSheet(): TemplateResult {
    const node = this.selectedNode;
    if (!node) return html``;
    const status = normalizeStatus(node.status);
    const isOnline = status === "online" || status === "degraded";
    return html`
      <sc-sheet .open=${!!this.selectedNode} @sc-close=${() => { this.selectedNode = null; }} heading="Node Details">
        <div class="detail-header">
          <span class="detail-status-dot ${status}"></span>
          <span class="detail-name">${node.id}</span>
          <sc-badge variant=${STATUS_VARIANT[status]}>${status}</sc-badge>
        </div>
        <div class="detail-grid">
          <span class="detail-label">Type</span>
          <span class="detail-value">${node.type ?? "\u2014"}</span>
          <span class="detail-label">Hostname</span>
          <span class="detail-value">${node.hostname ?? "\u2014"}</span>
          <span class="detail-label">Version</span>
          <span class="detail-value">${node.version ?? "\u2014"}</span>
          <span class="detail-label">Uptime</span>
          <span class="detail-value">${formatUptime(node.uptime_secs)}</span>
          <span class="detail-label">WebSocket Connections</span>
          <span class="detail-value">${node.ws_connections ?? 0}</span>
          ${node.cpu_percent != null ? html`
            <span class="detail-label">CPU Usage</span>
            <span class="detail-value">${node.cpu_percent}%</span>
          ` : nothing}
          ${node.memory_mb != null ? html`
            <span class="detail-label">Memory</span>
            <span class="detail-value">${node.memory_mb} MB</span>
          ` : nothing}
        </div>
        <div class="detail-actions">
          <sc-button size="sm" variant="secondary" .loading=${this.actionLoading} .disabled=${!isOnline}
            @click=${() => this._nodeAction(node.id, "restart")} aria-label="Restart node ${node.id}">
            Restart
          </sc-button>
          <sc-button size="sm" variant="destructive" .loading=${this.actionLoading} .disabled=${!isOnline}
            @click=${() => this._nodeAction(node.id, "disconnect")} aria-label="Disconnect node ${node.id}">
            Disconnect
          </sc-button>
        </div>
      </sc-sheet>
    `;
  }

  override render() {
    return html`
      <sc-page-hero>
        <sc-section-header heading="Nodes" description="Monitor connected instances, peripherals, and their health"></sc-section-header>
      </sc-page-hero>
      ${!this.error ? this._renderStats() : nothing}
      ${this._renderToolbar()}
      ${this.error
        ? html`<sc-empty-state .icon=${icons.warning} heading="Connection Error" .description=${this.error}></sc-empty-state>`
        : this._renderGrid()}
      ${this._renderDetailSheet()}
    `;
  }
}
