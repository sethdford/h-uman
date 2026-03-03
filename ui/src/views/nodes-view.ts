import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

interface NodeItem {
  id?: string;
  type?: string;
  status?: string;
  ws_connections?: number;
}

@customElement("sc-nodes-view")
export class ScNodesView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 1rem;
      flex-wrap: wrap;
      gap: 0.5rem;
    }
    h2 {
      margin: 0;
      font-size: 1.25rem;
      font-weight: 600;
      color: var(--sc-text);
    }
    .refresh-btn {
      padding: 0.5rem 1rem;
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      font-size: 0.875rem;
      cursor: pointer;
    }
    .refresh-btn:hover {
      background: var(--sc-border);
    }
    .refresh-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .nodes-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
      gap: 1rem;
      margin-bottom: 1.5rem;
    }
    .node-card {
      padding: 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
    }
    .node-header {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      margin-bottom: 0.5rem;
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
      font-size: 0.875rem;
      font-weight: 600;
      color: var(--sc-text);
      word-break: break-all;
    }
    .node-type {
      display: inline-block;
      padding: 0.2rem 0.5rem;
      font-size: 0.7rem;
      font-weight: 500;
      text-transform: lowercase;
      background: var(--sc-bg-elevated);
      color: var(--sc-text-muted);
      border-radius: 4px;
      margin-bottom: 0.5rem;
    }
    .node-info {
      font-size: 0.8125rem;
      color: var(--sc-text-muted);
    }
    .health-section {
      padding: 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      margin-top: 1rem;
    }
    .health-title {
      font-size: 0.875rem;
      font-weight: 600;
      color: var(--sc-text);
      margin-bottom: 0.5rem;
    }
    .health-status {
      font-size: 0.8125rem;
      color: var(--sc-text-muted);
    }
    .health-status.ok {
      color: var(--sc-success);
    }
    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-surface) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
      animation: sc-shimmer 1.5s ease-in-out infinite;
      border-radius: var(--sc-radius);
    }
    .skeleton-line {
      height: 1rem;
      margin-bottom: 0.75rem;
      border-radius: 4px;
    }
    .skeleton-card {
      height: 5rem;
      margin-bottom: 0.75rem;
    }
    .empty-state {
      text-align: center;
      padding: 3rem 1rem;
      color: var(--sc-text-muted);
    }
    .empty-icon {
      font-size: 2.5rem;
      margin-bottom: 1rem;
    }
    .empty-title {
      font-size: var(--sc-text-lg);
      font-weight: 600;
      color: var(--sc-text);
      margin: 0 0 0.5rem;
    }
    .empty-desc {
      font-size: var(--sc-text-sm);
      margin: 0;
      max-width: 24rem;
      margin-inline: auto;
    }
    .error {
      color: var(--sc-error);
      font-size: 0.875rem;
      margin-bottom: 1rem;
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
        <button class="refresh-btn" ?disabled=${this.loading} @click=${() => this.load()}>
          ${this.loading ? "Refreshing..." : "Refresh"}
        </button>
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
                <div class="empty-icon">🖥️</div>
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
                        <span class="status-dot ${this.statusDotClass(n.status)}"></span>
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
