import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";

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
    .nodes-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
    }
    .header-actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .staleness {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
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
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0.01ms !important;
        animation-iteration-count: 1 !important;
        transition-duration: 0.01ms !important;
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

  private _renderSkeleton(): TemplateResult {
    return html`
      <sc-skeleton variant="card" height="160px"></sc-skeleton>
      <sc-skeleton variant="card" height="160px"></sc-skeleton>
    `;
  }

  private _renderGrid(): TemplateResult {
    if (this.loading && this.nodes.length === 0) return this._renderSkeleton();
    if (this.nodes.length === 0) {
      return html`
        <sc-empty-state
          .icon=${icons.monitor}
          heading="No nodes connected"
          description="Connected devices and gateways will appear here."
        ></sc-empty-state>
      `;
    }
    return html`
      <div class="nodes-grid">
        ${this.nodes.map(
          (n) => html`
            <sc-card>
              <div class="node-header">
                <span class="status-dot ${this.statusDotClass(n.status)}" aria-hidden="true"></span>
                <span class="node-id">${n.id ?? "—"}</span>
              </div>
              <div class="node-type">${this.typeLabel(n.type)}</div>
              <div class="node-info">WebSocket connections: ${n.ws_connections ?? 0}</div>
            </sc-card>
          `,
        )}
      </div>
    `;
  }

  private _renderHealth(): TemplateResult {
    return html`
      <div class="health-section">
        <div class="health-title">Gateway health</div>
        <div class="health-status">
          ${this.loading && !this.healthStatus
            ? html`<span class="staleness">Loading...</span>`
            : html`<sc-badge variant=${this.healthStatus === "ok" ? "success" : "neutral"}
                >${this.healthStatus || "—"}</sc-badge
              >`}
        </div>
      </div>
    `;
  }

  override render() {
    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Nodes"
          description="Connected node instances and their status"
        ></sc-section-header>
      </sc-page-hero>
      <div class="header">
        <div class="header-actions">
          ${this.lastLoadedAt
            ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>`
            : nothing}
          <sc-button
            size="sm"
            .loading=${this.loading}
            @click=${() => this.load()}
            aria-label="Refresh nodes"
            >Refresh</sc-button
          >
        </div>
      </div>

      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            .description=${this.error}
          ></sc-empty-state>`
        : ""}
      ${!this.error ? this._renderGrid() : ""} ${this._renderHealth()}
    `;
  }
}
