import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface NodeItem {
  id?: string;
  type?: string;
  status?: string;
  ws_connections?: number;
}

@customElement("sc-nodes-view")
export class ScNodesView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .nodes-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
    }
    .node-card {
      padding: var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
    }
    .node-header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-sm);
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      flex-shrink: 0;
    }
    .status-dot.green {
      background: var(--sc-success);
    }
    .status-dot.yellow {
      background: var(--sc-warning);
    }
    .status-dot.red {
      background: var(--sc-error);
    }
    .node-id {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      word-break: break-all;
    }
    .node-type {
      display: inline-block;
      padding: var(--sc-space-xs) var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      text-transform: lowercase;
      background: var(--sc-bg-elevated);
      color: var(--sc-text-muted);
      border-radius: var(--sc-radius-sm);
      margin-bottom: var(--sc-space-sm);
    }
    .node-info {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .health-section {
      margin-top: var(--sc-space-md);
    }
    .health-title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-sm);
    }
    .health-status {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .health-status.ok {
      color: var(--sc-success);
    }
    .empty-state {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: var(--sc-space-2xl) var(--sc-space-lg);
    }
    .empty-icon {
      width: 2.5rem;
      height: 2.5rem;
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-md);
    }
    .empty-icon svg {
      width: 100%;
      height: 100%;
    }
    .empty-title {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: 0 0 var(--sc-space-xs);
    }
    .empty-desc {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      margin: 0;
    }
    @media (max-width: 768px) {
      .nodes-grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 480px) {
      .nodes-grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private nodes: NodeItem[] = [];
  @state() private healthStatus = "";
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const [nodesPayload, healthPayload] = await Promise.all([
        gw.request<{ nodes?: NodeItem[] }>("nodes.list", {}),
        gw.request<{ status?: string }>("health", {}),
      ]);
      this.nodes = nodesPayload?.nodes ?? [];
      this.healthStatus = healthPayload?.status ?? "unknown";
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load";
      this.nodes = [];
      this.healthStatus = "";
    } finally {
      this.loading = false;
    }
  }

  private statusDotClass(status: string | undefined): string {
    const s = (status ?? "").toLowerCase();
    if (s === "ok" || s === "healthy" || s === "connected" || s === "online") return "green";
    if (s === "degraded" || s === "warning") return "yellow";
    return "red";
  }

  private typeLabel(type: string | undefined): string {
    const t = (type ?? "").toLowerCase();
    if (t === "gateway" || t === "device" || t === "peripheral") return t;
    return t || "unknown";
  }

  override render() {
    return html`
      <div class="header">
        <h2>Nodes & Devices</h2>
        <div style="display:flex;align-items:center;gap:var(--sc-space-sm)">
          ${this.lastLoadedAt
            ? html`<span style="font-size:var(--sc-text-xs);color:var(--sc-text-muted)">
                Updated ${this.stalenessLabel}
              </span>`
            : nothing}
          <button class="refresh-btn" ?disabled=${this.loading} @click=${() => this.load()}>
            ${this.loading ? "Refreshing..." : "Refresh"}
          </button>
        </div>
      </div>

      ${this.error ? html`<p class="error">${this.error}</p>` : ""}
      ${this.loading && this.nodes.length === 0
        ? html`
            <div class="nodes-grid">
              <div class="node-card skeleton skeleton-card"></div>
            </div>
          `
        : this.nodes.length === 0
          ? html`
              <div class="empty-state">
                <div class="empty-icon">${icons.monitor}</div>
                <p class="empty-title">No nodes connected</p>
                <p class="empty-desc">Connected devices and gateways will appear here.</p>
              </div>
            `
          : html`
              <div class="nodes-grid">
                ${this.nodes.map(
                  (n) => html`
                    <div class="node-card">
                      <div class="node-header">
                        <span
                          class="status-dot ${this.statusDotClass(n.status)}"
                          aria-hidden="true"
                        ></span>
                        <span class="node-id">${n.id ?? "—"}</span>
                      </div>
                      <div class="node-type">${this.typeLabel(n.type)}</div>
                      <div class="node-info">WebSocket connections: ${n.ws_connections ?? 0}</div>
                    </div>
                  `,
                )}
              </div>
            `}

      <div class="health-section">
        <div class="health-title">Gateway health</div>
        <div class="health-status ${this.healthStatus === "ok" ? "ok" : ""}">
          ${this.loading && !this.healthStatus ? "Loading..." : this.healthStatus || "—"}
        </div>
      </div>
    `;
  }
}
