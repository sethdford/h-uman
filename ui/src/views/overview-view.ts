import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface HealthRes {
  status?: string;
}

interface CapabilitiesRes {
  version?: string;
  tools?: number;
  channels?: number;
  providers?: number;
}

interface ChannelItem {
  key?: string;
  label?: string;
  configured?: boolean;
  status?: string;
  build_enabled?: boolean;
}

interface SessionItem {
  key?: string;
  label?: string;
  created_at?: number;
  last_active?: number;
  turn_count?: number;
}

@customElement("sc-overview-view")
export class ScOverviewView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-lg);
      flex-wrap: wrap;
      gap: var(--sc-space-md);
    }
    .header h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-md);
    }
    .bento-card {
      animation: sc-slide-up var(--sc-duration-normal) var(--sc-ease-out) both;
    }
    .bento-card:nth-child(1) {
      animation-delay: 0ms;
    }
    .bento-card:nth-child(2) {
      animation-delay: 50ms;
    }
    .bento-card:nth-child(3) {
      animation-delay: 100ms;
    }
    .bento-card:nth-child(4) {
      animation-delay: 150ms;
    }
    .bento-card:nth-child(5) {
      animation-delay: 200ms;
    }
    .bento-card:nth-child(6) {
      animation-delay: 250ms;
    }
    .bento-card:nth-child(7) {
      animation-delay: 300ms;
    }
    .bento-card:nth-child(8) {
      animation-delay: 350ms;
    }
    .bento-card:nth-child(9) {
      animation-delay: 400ms;
    }
    .gateway-card {
      grid-column: span 2;
    }
    .channels-overview {
      grid-column: 1 / -1;
    }
    .recent-sessions {
      grid-column: 1 / -1;
    }
    .stat-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }
    .stat-value {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-bold);
      color: var(--sc-text);
    }
    .gateway-content {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-xs);
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      flex-shrink: 0;
    }
    .status-dot.operational {
      background: var(--sc-success);
    }
    .status-dot.offline {
      background: var(--sc-error);
    }
    .gateway-version {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .channels-inner {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
      gap: var(--sc-space-sm);
    }
    .channel-item {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--sc-space-sm);
    }
    .channel-name {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    .sessions-table {
      width: 100%;
      border-collapse: collapse;
    }
    .sessions-table th,
    .sessions-table td {
      padding: var(--sc-space-sm) var(--sc-space-md);
      text-align: left;
      border-bottom: 1px solid var(--sc-border);
      font-size: var(--sc-text-sm);
    }
    .sessions-table th {
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text-muted);
    }
    .sessions-table tr:last-child td {
      border-bottom: none;
    }
    .error {
      color: var(--sc-error);
      font-size: var(--sc-text-sm);
      margin-bottom: var(--sc-space-md);
    }
    .skeleton-grid {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-md);
    }
    .skeleton-gateway {
      grid-column: span 2;
    }
    .skeleton-full {
      grid-column: 1 / -1;
    }
    @media (max-width: 768px) {
      .bento {
        grid-template-columns: 1fr 1fr;
      }
      .gateway-card {
        grid-column: span 2;
      }
      .skeleton-grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 480px) {
      .bento {
        grid-template-columns: 1fr;
      }
      .gateway-card {
        grid-column: span 1;
      }
      .skeleton-grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private health: HealthRes = {};
  @state() private capabilities: CapabilitiesRes = {};
  @state() private channels: ChannelItem[] = [];
  @state() private sessions: SessionItem[] = [];
  @state() private loading = true;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.loading = false;
      this.error = "Not connected";
      return;
    }
    this.loading = true;
    this.error = "";
    try {
      const [healthRes, capRes, chRes, sessRes] = await Promise.all([
        gw.request<HealthRes>("health", {}).catch(() => ({})),
        gw.request<CapabilitiesRes>("capabilities", {}).catch(() => ({})),
        gw
          .request<{ channels?: ChannelItem[] }>("channels.status", {})
          .catch(() => ({ channels: [] })),
        gw
          .request<{ sessions?: SessionItem[] }>("sessions.list", {})
          .catch(() => ({ sessions: [] })),
      ]);
      this.health = healthRes as HealthRes;
      this.capabilities = capRes as CapabilitiesRes;
      const chPayload = chRes as { channels?: ChannelItem[] };
      this.channels = chPayload?.channels ?? [];
      const sessPayload = sessRes as { sessions?: SessionItem[] };
      this.sessions = sessPayload?.sessions ?? [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load overview";
      this.health = {};
      this.capabilities = {};
      this.channels = [];
      this.sessions = [];
    } finally {
      this.loading = false;
    }
  }

  private get gatewayOperational(): boolean {
    const s = (this.health.status ?? "").toLowerCase();
    return s === "ok" || s === "operational" || s === "healthy";
  }

  private get recentSessions(): SessionItem[] {
    const sorted = [...this.sessions].sort((a, b) => {
      const aTs = a.last_active ?? a.created_at ?? 0;
      const bTs = b.last_active ?? b.created_at ?? 0;
      return (bTs as number) - (aTs as number);
    });
    return sorted.slice(0, 5);
  }

  override render() {
    const cap = this.capabilities;
    const gwOk = this.gatewayOperational;

    if (this.loading) {
      return html`
        <div class="header">
          <h2>Overview</h2>
        </div>
        <div class="skeleton-grid bento">
          <div class="skeleton-gateway">
            <sc-skeleton variant="card" height="120px"></sc-skeleton>
          </div>
          <sc-skeleton variant="card" height="100px"></sc-skeleton>
          <sc-skeleton variant="card" height="100px"></sc-skeleton>
          <sc-skeleton variant="card" height="100px"></sc-skeleton>
          <sc-skeleton variant="card" height="100px"></sc-skeleton>
          <sc-skeleton variant="card" height="100px"></sc-skeleton>
          <div class="skeleton-full">
            <sc-skeleton variant="card" height="140px"></sc-skeleton>
          </div>
          <div class="skeleton-full">
            <sc-skeleton variant="card" height="180px"></sc-skeleton>
          </div>
        </div>
      `;
    }

    return html`
      <div class="header">
        <h2>Overview</h2>
        <sc-button variant="secondary" @click=${() => this.load()}>Refresh</sc-button>
      </div>
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      <div class="bento">
        <!-- 1. Gateway Status (2 cols) -->
        <sc-card hoverable class="bento-card gateway-card">
          <div class="stat-label">Gateway Status</div>
          <div class="gateway-content">
            <span class="status-dot ${gwOk ? "operational" : "offline"}" aria-hidden="true"></span>
            <span class="stat-value">${gwOk ? "Operational" : "Offline"}</span>
          </div>
          <div class="gateway-version">${cap.version ?? "—"}</div>
        </sc-card>

        <!-- 2. Providers -->
        <sc-card hoverable class="bento-card">
          <div class="stat-label">Providers</div>
          <div class="stat-value">${cap.providers ?? 0}</div>
        </sc-card>

        <!-- 3. Channels -->
        <sc-card hoverable class="bento-card">
          <div class="stat-label">Channels</div>
          <div class="stat-value">${cap.channels ?? 0}</div>
        </sc-card>

        <!-- 4. Tools -->
        <sc-card hoverable class="bento-card">
          <div class="stat-label">Tools</div>
          <div class="stat-value">${cap.tools ?? 0}</div>
        </sc-card>

        <!-- 5. Active Sessions -->
        <sc-card hoverable class="bento-card">
          <div class="stat-label">Active Sessions</div>
          <div class="stat-value">${this.sessions.length}</div>
        </sc-card>

        <!-- 6. Channels Overview (full width) -->
        <sc-card hoverable class="bento-card channels-overview">
          <div class="stat-label" style="margin-bottom: var(--sc-space-sm);">Channels Overview</div>
          ${this.channels.length === 0
            ? html`
                <sc-empty-state
                  icon="📡"
                  heading="No channels"
                  description="Configure channels in the Channels view."
                ></sc-empty-state>
              `
            : html`
                <div class="channels-inner">
                  ${this.channels.map(
                    (ch) => html`
                      <div class="channel-item">
                        <span class="channel-name">${ch.label ?? ch.key ?? "unnamed"}</span>
                        <sc-badge variant=${ch.configured ? "success" : "neutral"} dot
                          >${ch.status ?? (ch.configured ? "Configured" : "—")}</sc-badge
                        >
                      </div>
                    `,
                  )}
                </div>
              `}
        </sc-card>

        <!-- 7. Recent Sessions (full width) -->
        <sc-card hoverable class="bento-card recent-sessions">
          <div class="stat-label" style="margin-bottom: var(--sc-space-sm);">Recent Sessions</div>
          ${this.recentSessions.length === 0
            ? html`
                <sc-empty-state
                  icon="🗨️"
                  heading="No sessions yet"
                  description="Start a conversation to see your sessions here."
                ></sc-empty-state>
              `
            : html`
                <table class="sessions-table">
                  <thead>
                    <tr>
                      <th>Session</th>
                      <th>Turns</th>
                      <th>Last active</th>
                    </tr>
                  </thead>
                  <tbody>
                    ${this.recentSessions.map(
                      (s) => html`
                        <tr>
                          <td>${s.label ?? s.key ?? "unnamed"}</td>
                          <td>${s.turn_count ?? 0}</td>
                          <td>${formatDate(s.last_active)}</td>
                        </tr>
                      `,
                    )}
                  </tbody>
                </table>
              `}
        </sc-card>
      </div>
    `;
  }
}
