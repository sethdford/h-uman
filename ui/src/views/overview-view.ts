import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { friendlyError } from "../utils/friendly-error.js";
import { icons } from "../icons.js";
import { observeAllCards, unobserveAllCards } from "../utils/scroll-entrance.js";
import type { ActivityEvent } from "../components/hu-activity-feed.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import "../components/hu-welcome.js";
import "../components/hu-welcome-card.js";
import "../components/hu-tooltip.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-overview-stats.js";
import "../components/hu-sessions-table.js";
import "../components/hu-activity-timeline.js";
import "../components/hu-status-dot.js";
import "../components/hu-chart.js";

interface HealthRes {
  status?: string;
  uptime_secs?: number;
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
  peak_rss_mb?: number;
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

@customElement("hu-overview-view")
export class ScOverviewView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-overview;
      display: block;
      max-width: 75rem;
      padding: var(--hu-space-lg) var(--hu-space-xl);
    }

    /* ── Hero zone ────────────────────────────────────── */

    .hero-left {
      display: flex;
      align-items: center;
      gap: var(--hu-space-lg);
      min-width: 0;
    }

    .hero-status {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .hero-title {
      margin: 0;
      font-size: var(--hu-text-3xl);
      font-weight: var(--hu-weight-bold);
      letter-spacing: var(--hu-tracking-hero);
      color: var(--hu-text);
      line-height: var(--hu-leading-tight);
    }

    .hero-meta {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .hero-meta .update-link {
      color: var(--hu-accent-text, var(--hu-accent));
      text-decoration: none;
      font-weight: var(--hu-weight-medium);
    }

    .hero-meta .update-link:hover {
      text-decoration: underline;
    }

    .hero-actions {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .staleness {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    /* ── Detail zone (asymmetric bento) ────────────────── */

    .details {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xl);
    }

    .bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      grid-template-areas:
        "activity activity channels channels"
        "activity activity sessions sessions";
      gap: var(--hu-space-xl);
    }

    .bento .activity {
      grid-area: activity;
    }

    .bento .channels {
      grid-area: channels;
    }

    .bento .sessions {
      grid-area: sessions;
    }

    .section-label {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-semibold);
      letter-spacing: var(--hu-tracking-xs);
      text-transform: uppercase;
      color: var(--hu-accent-text, var(--hu-accent));
      margin-bottom: var(--hu-space-sm);
    }

    .channels-with-chart {
      display: flex;
      gap: var(--hu-space-lg);
      align-items: flex-start;
    }

    .channels-with-chart hu-chart {
      flex-shrink: 0;
    }

    .channels-inner {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(var(--hu-sidebar-width), 1fr));
      gap: var(--hu-space-sm);
      flex: 1;
      min-width: 0;
    }

    .channel-item {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-sm);
    }

    .channel-name {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
    }

    /* ── Skeleton ─────────────────────────────────────── */

    .skeleton-hero {
      margin-bottom: var(--hu-space-2xl);
    }

    .skeleton-metrics {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--hu-space-lg);
      margin-bottom: var(--hu-space-2xl);
    }

    .skeleton-bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      grid-template-areas:
        "activity activity channels channels"
        "activity activity sessions sessions";
      gap: var(--hu-space-xl);
    }

    .skeleton-bento .activity {
      grid-area: activity;
    }

    .skeleton-bento .channels {
      grid-area: channels;
    }

    .skeleton-bento .sessions {
      grid-area: sessions;
    }

    /* ── Responsive ───────────────────────────────────── */

    @media (max-width: var(--hu-breakpoint-md)) /* --hu-breakpoint-md */ {
      .bento {
        grid-template-columns: 1fr;
        grid-template-areas:
          "activity"
          "channels"
          "sessions";
      }
      .skeleton-bento {
        grid-template-columns: 1fr;
        grid-template-areas:
          "activity"
          "channels"
          "sessions";
      }
      .skeleton-metrics {
        grid-template-columns: 1fr 1fr;
      }
      .channels-with-chart {
        flex-direction: column;
      }
    }

    @media (max-width: var(--hu-breakpoint-sm)) /* --hu-breakpoint-sm */ {
      :host {
        padding: var(--hu-space-md) var(--hu-space-lg);
      }
      .skeleton-metrics {
        grid-template-columns: 1fr;
      }
    }

    /* ── Quick actions row ───────────────────────────────── */

    .quick-actions {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--hu-space-lg);
      margin-bottom: var(--hu-space-2xl);
    }
    .quick-action-card {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-lg);
    }
    .quick-action-card .icon-wrap {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-2xl);
      height: var(--hu-icon-2xl);
      color: var(--hu-accent);
    }
    .quick-action-card .icon-wrap svg {
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
    }
    .quick-action-card .label {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
    }
    @media (max-width: var(--hu-breakpoint-md)) /* --hu-breakpoint-md */ {
      .quick-actions {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
        transition-duration: 0s !important;
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
    const detail = e.detail as { event: string; payload?: unknown };
    if (
      detail.event === "activity" &&
      typeof detail.payload === "object" &&
      detail.payload != null
    ) {
      this.activityEvents = [detail.payload as ActivityEvent, ...this.activityEvents].slice(0, 20);
    }
  }) as EventListener;

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway?.addEventListener("gateway", this._gwEventHandler);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this.gateway?.removeEventListener("gateway", this._gwEventHandler);
    if (this.shadowRoot) unobserveAllCards(this.shadowRoot);
  }

  protected override onGatewaySwapped(
    previous: GatewayAwareLitElement["gateway"],
    current: NonNullable<GatewayAwareLitElement["gateway"]>,
  ): void {
    previous?.removeEventListener("gateway", this._gwEventHandler);
    current.addEventListener("gateway", this._gwEventHandler);
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
      this.channels = Array.isArray(chPayload?.channels) ? chPayload.channels : [];
      const sessPayload = sessRes as { sessions?: SessionItem[] };
      this.sessions = Array.isArray(sessPayload?.sessions) ? sessPayload.sessions : [];
      this.updateInfo = (updateRes as UpdateInfo) ?? {};
      const actPayload = actRes as { events?: ActivityEvent[] };
      if (
        Array.isArray(actPayload?.events) &&
        actPayload.events.length > 0 &&
        this.activityEvents.length === 0
      ) {
        this.activityEvents = actPayload.events;
      }
      this._updateWelcome();
    } catch (e) {
      this.error = friendlyError(e);
      this.health = {};
      this.capabilities = {};
      this.channels = [];
      this.sessions = [];
    } finally {
      this.loading = false;
    }
  }

  private _updateWelcome(): void {
    const welcome = this.shadowRoot?.querySelector("hu-welcome") as
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

  private get _channelDoughnutData() {
    const configured = this.channels.filter((c) => c.configured).length;
    const unconfigured = this.channels.length - configured;
    return {
      labels: ["Configured", "Unconfigured"],
      datasets: [{ data: [configured, unconfigured] }],
    };
  }

  private get _onboarded(): boolean {
    return localStorage.getItem("hu-onboarded") === "true";
  }

  /* ── Render: top-level ──────────────────────────────── */

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      ${this.error
        ? html`<hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <hu-button variant="primary" @click=${() => this.load()}> Retry </hu-button>
          </hu-empty-state>`
        : nothing}
      ${this._renderHero()} ${this._renderMetrics()} ${this._renderQuickActions()}
      ${this._renderDetails()}
    `;
  }

  /* ── Hero zone ──────────────────────────────────────── */

  private _renderHero() {
    if (!this._onboarded) {
      return html`
        <hu-page-hero role="region" aria-label="Overview">
          <hu-welcome-card></hu-welcome-card>
          <hu-welcome></hu-welcome>
        </hu-page-hero>
      `;
    }

    const gwOk = this.gatewayOperational;
    const cap = this.capabilities;

    return html`
      <hu-page-hero role="region" aria-label="Overview">
        <hu-section-header heading="Overview" description="Your AI assistant at a glance">
          <div class="hero-actions">
            ${this.lastLoadedAt
              ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>`
              : nothing}
            <hu-tooltip text="Reload all dashboard data" position="bottom">
              <hu-button variant="secondary" @click=${() => this.load()}>Refresh</hu-button>
            </hu-tooltip>
          </div>
        </hu-section-header>
        <div class="hero-inner">
          <div class="hero-left">
            <hu-tooltip text=${gwOk ? "All subsystems responding" : "Gateway is unreachable"}>
              <hu-status-dot status=${gwOk ? "operational" : "offline"} size="md"></hu-status-dot>
            </hu-tooltip>
            <div class="hero-status">
              <div class="hero-meta">
                <span>${cap.version ?? "h-uman"}</span>
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
        </div>
      </hu-page-hero>
    `;
  }

  /* ── Stats row ─────────────────────────────────────── */

  /** Deterministic mock trend data for sparklines (no flicker on re-render). */
  private _mockTrendData(base: number, variance: number, points = 12): number[] {
    const out: number[] = [];
    let v = base;
    for (let i = 0; i < points; i++) {
      const wave = Math.sin((i / points) * Math.PI * 2) * 0.5 + 0.5;
      v = Math.max(0, base + (wave - 0.3) * variance);
      out.push(Math.round(v * 10) / 10);
    }
    return out;
  }

  private _renderMetrics() {
    const cap = this.capabilities;
    const providers = cap.providers ?? 0;
    const channels = cap.channels ?? 0;
    const tools = cap.tools ?? 0;
    const memoryMb = cap.peak_rss_mb ?? 5.9;
    const memoryStr = `${memoryMb.toFixed(1)} MB`;
    const metrics = [
      {
        label: "Providers",
        value: providers,
        sparklineData: this._mockTrendData(providers || 9, 2),
      },
      {
        label: "Channels",
        value: channels,
        sparklineData: this._mockTrendData(channels || 34, 3),
      },
      {
        label: "Tools",
        value: tools,
        sparklineData: this._mockTrendData(tools || 67, 4),
      },
      {
        label: "Memory",
        valueStr: memoryStr,
        sparklineData: this._mockTrendData(memoryMb, 0.8),
      },
    ];
    const metricRowItems = [
      { label: "Sessions Today", value: String(this.sessions.length) },
      {
        label: "Channels Active",
        value: String(this.channels.filter((c) => c.configured).length),
      },
      {
        label: "Status",
        value: this.gatewayOperational ? "Healthy" : "Offline",
        accent: (this.gatewayOperational ? "success" : "error") as "success" | "error",
      },
    ];
    return html`
      <hu-overview-stats .metrics=${metrics} .metricRowItems=${metricRowItems}></hu-overview-stats>
    `;
  }

  /* ── Quick actions row ───────────────────────────────── */

  private _renderQuickActions() {
    if (!this._onboarded) return nothing;
    const actions = [
      { label: "Chat", icon: icons["message-square"], target: "chat" },
      { label: "New Automation", icon: icons.clock, target: "automations" },
      { label: "Voice", icon: icons.mic, target: "voice" },
    ];
    return html`
      <div class="quick-actions">
        ${actions.map(
          (a) => html`
            <hu-card
              glass
              clickable
              surface="high"
              class="quick-action-card"
              role="button"
              tabindex="0"
              aria-label=${`Go to ${a.label}`}
              @click=${() => this._navigate(a.target)}
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  this._navigate(a.target);
                }
              }}
            >
              <div class="icon-wrap" aria-hidden="true">${a.icon}</div>
              <span class="label">${a.label}</span>
            </hu-card>
          `,
        )}
      </div>
    `;
  }

  /* ── Detail zone ────────────────────────────────────── */

  private _renderDetails() {
    return html`
      <div class="details">
        <div class="bento">
          <hu-card hoverable accent surface="high" class="activity">
            <hu-activity-timeline .events=${this.activityEvents}></hu-activity-timeline>
          </hu-card>

          <hu-card hoverable accent surface="high" class="channels">
            <div class="section-label">Channels</div>
            ${this.channels.length === 0
              ? html`
                  <hu-empty-state
                    .icon=${icons.radio}
                    heading="No channels yet"
                    description="Connect Telegram, Discord, Slack, or any messaging platform."
                  >
                    <hu-button variant="primary" @click=${() => this._navigate("channels")}>
                      Configure a Channel
                    </hu-button>
                  </hu-empty-state>
                `
              : html`
                  <div class="channels-with-chart">
                    <hu-chart
                      type="doughnut"
                      .data=${this._channelDoughnutData}
                      height=${100}
                    ></hu-chart>
                    <div class="channels-inner">
                      ${this.channels.map(
                        (ch) => html`
                          <div class="channel-item">
                            <span class="channel-name">${ch.label ?? ch.key ?? "unnamed"}</span>
                            <hu-badge variant=${ch.configured ? "success" : "neutral"} dot>
                              ${ch.status ?? (ch.configured ? "Configured" : "\u2014")}
                            </hu-badge>
                          </div>
                        `,
                      )}
                    </div>
                  </div>
                `}
          </hu-card>

          <hu-card hoverable accent surface="high" class="sessions">
            <div class="section-label">Recent Sessions</div>
            <hu-sessions-table
              .sessions=${this.recentSessions}
              @session-select=${(e: CustomEvent<{ key: string }>) =>
                this.dispatchEvent(
                  new CustomEvent("navigate", {
                    detail: "chat:" + (e.detail.key ?? ""),
                    bubbles: true,
                    composed: true,
                  }),
                )}
            ></hu-sessions-table>
          </hu-card>
        </div>
      </div>
    `;
  }

  /* ── Skeleton (loading state) ───────────────────────── */

  private _renderSkeleton() {
    return html`
      <div class="skeleton-hero">
        <hu-skeleton variant="card" height="100px"></hu-skeleton>
      </div>
      <div class="skeleton-metrics">
        <hu-skeleton variant="stat-card"></hu-skeleton>
        <hu-skeleton variant="stat-card"></hu-skeleton>
        <hu-skeleton variant="stat-card"></hu-skeleton>
        <hu-skeleton variant="stat-card"></hu-skeleton>
      </div>
      <div class="skeleton-bento">
        <hu-skeleton variant="card" height="280px" class="activity"></hu-skeleton>
        <hu-skeleton variant="card" height="140px" class="channels"></hu-skeleton>
        <hu-skeleton variant="card" height="140px" class="sessions"></hu-skeleton>
      </div>
    `;
  }
}
