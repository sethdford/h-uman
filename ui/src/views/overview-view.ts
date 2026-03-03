import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface HealthRes {
  status?: string;
}

interface UpdateInfo {
  available?: boolean;
  current_version?: string;
  latest_version?: string;
  url?: string;
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
      max-width: 1200px;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
      flex-wrap: wrap;
      gap: var(--sc-space-md);
    }
    .header h2 {
      margin: 0;
      font-size: var(--sc-text-2xl);
      font-weight: var(--sc-weight-bold);
      letter-spacing: -0.02em;
      color: var(--sc-text);
    }
    .bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-xl);
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
      letter-spacing: 0.02em;
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }
    .stat-value {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-bold);
      font-variant-numeric: tabular-nums;
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
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
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
      gap: var(--sc-space-xl);
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
  @state() private updateInfo: UpdateInfo = {};

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
      const [healthRes, capRes, chRes, sessRes, updateRes] = await Promise.all([
        gw.request<HealthRes>("health", {}).catch(() => ({})),
        gw.request<CapabilitiesRes>("capabilities", {}).catch(() => ({})),
        gw
          .request<{ channels?: ChannelItem[] }>("channels.status", {})
          .catch(() => ({ channels: [] })),
        gw
          .request<{ sessions?: SessionItem[] }>("sessions.list", {})
          .catch(() => ({ sessions: [] })),
        gw.request<UpdateInfo>("update.check", {}).catch(() => ({})),
      ]);
      this.health = healthRes as HealthRes;
      this.capabilities = capRes as CapabilitiesRes;
      const chPayload = chRes as { channels?: ChannelItem[] };
      this.channels = chPayload?.channels ?? [];
      const sessPayload = sessRes as { sessions?: SessionItem[] };
      this.sessions = sessPayload?.sessions ?? [];
      this.updateInfo = (updateRes as UpdateInfo) ?? {};
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
      <div class="bento sc-stagger">
        <!-- 1. Gateway Status (2 cols) -->
        <sc-card hoverable class="gateway-card">
          <div class="stat-label">Gateway Status</div>
          <div class="gateway-content">
            <span class="status-dot ${gwOk ? "operational" : "offline"}" aria-hidden="true"></span>
            <span class="stat-value">${gwOk ? "Operational" : "Offline"}</span>
          </div>
          <div class="gateway-version">${cap.version ?? "—"}</div>
        </sc-card>

        <!-- 2. Providers -->
        <sc-card hoverable>
          <div class="stat-label">Providers</div>
          <div class="stat-value">${cap.providers ?? 0}</div>
        </sc-card>

        <!-- 3. Channels -->
        <sc-card hoverable>
          <div class="stat-label">Channels</div>
          <div class="stat-value">${cap.channels ?? 0}</div>
        </sc-card>

        <!-- 4. Tools -->
        <sc-card hoverable>
          <div class="stat-label">Tools</div>
          <div class="stat-value">${cap.tools ?? 0}</div>
        </sc-card>

        <!-- 5. Active Sessions -->
        <sc-card hoverable>
          <div class="stat-label">Active Sessions</div>
          <div class="stat-value">${this.sessions.length}</div>
        </sc-card>

        ${this.updateInfo.available
          ? html`
              <sc-card hoverable>
                <div class="stat-label">Update Available</div>
                <div
                  class="stat-value"
                  style="font-size: var(--sc-text-base); color: var(--sc-accent);"
                >
                  ${this.updateInfo.current_version ?? "—"} →
                  ${this.updateInfo.latest_version ?? "—"}
                </div>
              </sc-card>
            `
          : nothing}

        <!-- 6. Channels Overview (full width) -->
        <sc-card hoverable class="channels-overview">
          <div class="stat-label" style="margin-bottom: var(--sc-space-sm);">Channels Overview</div>
          ${this.channels.length === 0
            ? html`
                <sc-empty-state
                  .icon=${icons.radio}
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
        <sc-card hoverable class="recent-sessions">
          <div class="stat-label" style="margin-bottom: var(--sc-space-sm);">Recent Sessions</div>
          ${this.recentSessions.length === 0
            ? html`
                <sc-empty-state
                  .icon=${icons["chat-circle"]}
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
