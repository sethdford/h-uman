import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { observeAllCards } from "../utils/scroll-entrance.js";
import type { ActivityEvent } from "../components/sc-activity-feed.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";
import "../components/sc-animated-number.js";
import "../components/sc-welcome.js";
import "../components/sc-welcome-card.js";
import "../components/sc-tooltip.js";
import "../components/sc-activity-feed.js";

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
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      display: block;
      max-width: 1200px;
      padding: var(--sc-space-lg) var(--sc-space-xl);
    }

    /* ── Hero zone ────────────────────────────────────── */

    .hero {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      padding: var(--sc-space-xl) var(--sc-space-2xl);
      margin-bottom: var(--sc-space-2xl, 2rem);
      background-image: var(--sc-hero-gradient);
      border-radius: var(--sc-radius-xl, 16px);
      border: 1px solid var(--sc-border-subtle, var(--sc-border));
    }

    .hero-left {
      display: flex;
      align-items: center;
      gap: var(--sc-space-lg);
      min-width: 0;
    }

    .hero-status {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .hero-title {
      margin: 0;
      font-size: clamp(1.5rem, 2.5vw, 2rem);
      font-weight: var(--sc-weight-bold, 700);
      letter-spacing: -0.03em;
      color: var(--sc-text);
      line-height: 1.1;
    }

    .hero-meta {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .hero-meta .update-link {
      color: var(--sc-accent-text, var(--sc-accent));
      text-decoration: none;
      font-weight: var(--sc-weight-medium);
    }

    .hero-meta .update-link:hover {
      text-decoration: underline;
    }

    .hero-actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }

    .staleness {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .status-dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      flex-shrink: 0;
    }

    .status-dot.operational {
      background: var(--sc-success);
      box-shadow: 0 0 6px var(--sc-success);
      animation: sc-status-pulse 2s ease-in-out infinite;
    }

    .status-dot.offline {
      background: var(--sc-error);
    }

    @keyframes sc-status-pulse {
      0%,
      100% {
        box-shadow: 0 0 4px var(--sc-success);
        opacity: 1;
      }
      50% {
        box-shadow: 0 0 12px var(--sc-success);
        opacity: 0.8;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .status-dot.operational {
        animation: none;
      }
    }

    /* ── Metrics zone ─────────────────────────────────── */

    .metrics {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-lg, 1.5rem);
      margin-bottom: var(--sc-space-2xl, 2rem);
    }

    .stat-label {
      font-size: 0.6875rem;
      font-weight: var(--sc-weight-semibold, 600);
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--sc-accent-text, var(--sc-accent));
      margin-bottom: var(--sc-space-xs);
    }

    .stat-value {
      font-size: clamp(1.5rem, 2.5vw, 2rem);
      font-weight: var(--sc-weight-bold, 700);
      letter-spacing: -0.04em;
      font-variant-numeric: tabular-nums;
      color: var(--sc-text);
      line-height: 1.1;
      animation: sc-overshoot-in var(--sc-duration-moderate) var(--sc-spring-out) backwards;
    }

    .stat-desc {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-top: var(--sc-space-2xs);
    }

    /* ── Detail zone ──────────────────────────────────── */

    .details {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xl, 1.5rem);
    }

    .details-row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-xl);
    }

    .section-label {
      font-size: 0.6875rem;
      font-weight: var(--sc-weight-semibold, 600);
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--sc-accent-text, var(--sc-accent));
      margin-bottom: var(--sc-space-sm);
    }

    .channels-inner {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
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

    /* ── Error ────────────────────────────────────────── */

    .error {
      color: var(--sc-error);
      font-size: var(--sc-text-sm);
      margin-bottom: var(--sc-space-md);
    }

    /* ── Skeleton ─────────────────────────────────────── */

    .skeleton-hero {
      margin-bottom: var(--sc-space-2xl, 2rem);
    }

    .skeleton-metrics {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-lg);
      margin-bottom: var(--sc-space-2xl, 2rem);
    }

    .skeleton-details {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-xl);
    }

    .skeleton-full {
      grid-column: 1 / -1;
    }

    /* ── Responsive ───────────────────────────────────── */

    @media (max-width: 768px) {
      .metrics {
        grid-template-columns: 1fr 1fr;
      }
      .details-row {
        grid-template-columns: 1fr;
      }
      .skeleton-metrics {
        grid-template-columns: 1fr 1fr;
      }
      .skeleton-details {
        grid-template-columns: 1fr;
      }
    }

    @media (max-width: 480px) {
      .metrics {
        grid-template-columns: 1fr;
      }
      .hero {
        padding: var(--sc-space-lg);
        flex-direction: column;
        align-items: flex-start;
      }
      .skeleton-metrics {
        grid-template-columns: 1fr;
      }
    }
  `;

  override updated(): void {
    if (!this.loading && this.shadowRoot) {
      observeAllCards(this.shadowRoot);
    }
  }

  @state() private health: HealthRes = {};
  @state() private capabilities: CapabilitiesRes = {};
  @state() private channels: ChannelItem[] = [];
  @state() private sessions: SessionItem[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private updateInfo: UpdateInfo = {};
  @state() private activityEvents: ActivityEvent[] = [];
  private _gwEventHandler = ((e: CustomEvent) => {
    const detail = e.detail as { event: string; payload: ActivityEvent };
    if (detail.event === "activity" && detail.payload) {
      this.activityEvents = [detail.payload, ...this.activityEvents].slice(0, 20);
    }
  }) as EventListener;

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway?.addEventListener("gateway", this._gwEventHandler);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this.gateway?.removeEventListener("gateway", this._gwEventHandler);
  }

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
      const [healthRes, capRes, chRes, sessRes, updateRes, actRes] = await Promise.all([
        gw.request<HealthRes>("health", {}).catch(() => ({})),
        gw.request<CapabilitiesRes>("capabilities", {}).catch(() => ({})),
        gw
          .request<{ channels?: ChannelItem[] }>("channels.status", {})
          .catch(() => ({ channels: [] })),
        gw
          .request<{ sessions?: SessionItem[] }>("sessions.list", {})
          .catch(() => ({ sessions: [] })),
        gw.request<UpdateInfo>("update.check", {}).catch(() => ({})),
        gw
          .request<{ events?: ActivityEvent[] }>("activity.recent", {})
          .catch(() => ({ events: [] })),
      ]);
      this.health = healthRes as HealthRes;
      this.capabilities = capRes as CapabilitiesRes;
      const chPayload = chRes as { channels?: ChannelItem[] };
      this.channels = chPayload?.channels ?? [];
      const sessPayload = sessRes as { sessions?: SessionItem[] };
      this.sessions = sessPayload?.sessions ?? [];
      this.updateInfo = (updateRes as UpdateInfo) ?? {};
      const actPayload = actRes as { events?: ActivityEvent[] };
      if (actPayload?.events?.length && this.activityEvents.length === 0) {
        this.activityEvents = actPayload.events;
      }
      this._updateWelcome();
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

  private _updateWelcome(): void {
    const welcome = this.shadowRoot?.querySelector("sc-welcome") as
      | (HTMLElement & { markStep: (k: string) => void })
      | null;
    if (!welcome) return;
    if (this.gateway?.status === "connected") welcome.markStep("connect");
    if (this.gatewayOperational) welcome.markStep("health");
    if (this.channels.some((ch) => ch.configured)) welcome.markStep("channel");
    if (this.sessions.length > 0) welcome.markStep("chat");
  }

  private _navigate(tab: string): void {
    this.dispatchEvent(new CustomEvent("navigate", { detail: tab, bubbles: true, composed: true }));
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

  private get _onboarded(): boolean {
    return localStorage.getItem("sc-onboarded") === "true";
  }

  /* ── Render: top-level ──────────────────────────────── */

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing} ${this._renderHero()}
      ${this._renderMetrics()} ${this._renderDetails()}
    `;
  }

  /* ── Hero zone ──────────────────────────────────────── */

  private _renderHero() {
    if (!this._onboarded) {
      return html`
        <sc-welcome-card></sc-welcome-card>
        <sc-welcome></sc-welcome>
      `;
    }

    const gwOk = this.gatewayOperational;
    const cap = this.capabilities;

    return html`
      <div class="hero">
        <div class="hero-left">
          <sc-tooltip text=${gwOk ? "All subsystems responding" : "Gateway is unreachable"}>
            <span class="status-dot ${gwOk ? "operational" : "offline"}" aria-hidden="true"></span>
          </sc-tooltip>
          <div class="hero-status">
            <h2 class="hero-title">${gwOk ? "Operational" : "Offline"}</h2>
            <div class="hero-meta">
              <span>${cap.version ?? "SeaClaw"}</span>
              ${this.updateInfo.available
                ? html`<span>&middot;</span>
                    <a
                      class="update-link"
                      href=${this.updateInfo.url ?? "#"}
                      target="_blank"
                      rel="noopener"
                    >
                      Update to ${this.updateInfo.latest_version}
                    </a>`
                : nothing}
            </div>
          </div>
        </div>
        <div class="hero-actions">
          ${this.lastLoadedAt
            ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>`
            : nothing}
          <sc-tooltip text="Reload all dashboard data" position="bottom">
            <sc-button variant="secondary" @click=${() => this.load()}>Refresh</sc-button>
          </sc-tooltip>
        </div>
      </div>
    `;
  }

  /* ── Metrics zone ───────────────────────────────────── */

  private _renderMetrics() {
    const cap = this.capabilities;
    const metrics = [
      { label: "Providers", value: cap.providers ?? 0, desc: "AI model backends" },
      { label: "Channels", value: cap.channels ?? 0, desc: "Messaging integrations" },
      { label: "Tools", value: cap.tools ?? 0, desc: "Available capabilities" },
      { label: "Sessions", value: this.sessions.length, desc: "Active conversations" },
    ];

    return html`
      <div class="metrics sc-stagger">
        ${metrics.map(
          (m) => html`
            <sc-card hoverable accent>
              <div class="stat-label">${m.label}</div>
              <div class="stat-value">
                <sc-animated-number .value=${m.value}></sc-animated-number>
              </div>
              <div class="stat-desc">${m.desc}</div>
            </sc-card>
          `,
        )}
      </div>
    `;
  }

  /* ── Detail zone ────────────────────────────────────── */

  private _renderDetails() {
    return html`
      <div class="details">
        <div class="details-row">
          <sc-card hoverable accent>
            <div class="section-label">Live Activity</div>
            <sc-activity-feed .events=${this.activityEvents}></sc-activity-feed>
          </sc-card>

          <sc-card hoverable accent>
            <div class="section-label">Channels</div>
            ${this.channels.length === 0
              ? html`
                  <sc-empty-state
                    .icon=${icons.radio}
                    heading="No channels yet"
                    description="Connect Telegram, Discord, Slack, or any messaging platform."
                  >
                    <sc-button variant="primary" @click=${() => this._navigate("channels")}>
                      Configure a Channel
                    </sc-button>
                  </sc-empty-state>
                `
              : html`
                  <div class="channels-inner">
                    ${this.channels.map(
                      (ch) => html`
                        <div class="channel-item">
                          <span class="channel-name">${ch.label ?? ch.key ?? "unnamed"}</span>
                          <sc-badge variant=${ch.configured ? "success" : "neutral"} dot>
                            ${ch.status ?? (ch.configured ? "Configured" : "\u2014")}
                          </sc-badge>
                        </div>
                      `,
                    )}
                  </div>
                `}
          </sc-card>
        </div>

        <sc-card hoverable accent>
          <div class="section-label">Recent Sessions</div>
          ${this.recentSessions.length === 0
            ? html`
                <sc-empty-state
                  .icon=${icons["chat-circle"]}
                  heading="No conversations yet"
                  description="Start your first chat to see SeaClaw in action."
                >
                  <sc-button variant="primary" @click=${() => this._navigate("chat")}>
                    Start a Conversation
                  </sc-button>
                </sc-empty-state>
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

  /* ── Skeleton (loading state) ───────────────────────── */

  private _renderSkeleton() {
    return html`
      <div class="skeleton-hero">
        <sc-skeleton variant="card" height="100px"></sc-skeleton>
      </div>
      <div class="skeleton-metrics">
        <sc-skeleton variant="stat-card"></sc-skeleton>
        <sc-skeleton variant="stat-card"></sc-skeleton>
        <sc-skeleton variant="stat-card"></sc-skeleton>
        <sc-skeleton variant="stat-card"></sc-skeleton>
      </div>
      <div class="skeleton-details">
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
        <div class="skeleton-full">
          <sc-skeleton variant="card" height="160px"></sc-skeleton>
        </div>
      </div>
    `;
  }
}
