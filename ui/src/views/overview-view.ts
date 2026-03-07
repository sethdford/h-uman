import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate, formatRelative } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { observeAllCards, unobserveAllCards } from "../utils/scroll-entrance.js";
import type { ActivityEvent } from "../components/sc-activity-feed.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";
import "../components/sc-welcome.js";
import "../components/sc-welcome-card.js";
import "../components/sc-tooltip.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-stat-card.js";
import "../components/sc-metric-row.js";
import "../components/sc-timeline.js";
import "../components/sc-chart.js";

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

@customElement("sc-overview-view")
export class ScOverviewView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-overview;
      display: block;
      max-width: 75rem;
      padding: var(--sc-space-lg) var(--sc-space-xl);
    }

    /* ── Hero zone ────────────────────────────────────── */

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
      width: 0.625rem;
      height: 0.625rem;
      border-radius: 50%;
      flex-shrink: 0;
    }

    .status-dot.operational {
      background: var(--sc-success);
      box-shadow: 0 0 var(--sc-space-sm) var(--sc-success);
      animation: sc-status-pulse var(--sc-duration-slow) ease-in-out infinite;
    }

    .status-dot.offline {
      background: var(--sc-error);
    }

    @keyframes sc-status-pulse {
      0%,
      100% {
        box-shadow: 0 0 var(--sc-space-xs) var(--sc-success);
        opacity: 1;
      }
      50% {
        box-shadow: 0 0 var(--sc-space-md) var(--sc-success);
        opacity: 0.8;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .status-dot.operational {
        animation: none;
      }
    }

    /* ── Stats row ───────────────────────────────────── */

    .metrics-block {
      margin-bottom: var(--sc-space-2xl);
    }

    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
    }

    /* ── Detail zone (asymmetric bento) ────────────────── */

    .details {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xl, 1.5rem);
    }

    .bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      grid-template-areas:
        "activity activity channels channels"
        "activity activity sessions sessions";
      gap: var(--sc-space-xl);
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

    .activity-sparkline {
      margin-bottom: var(--sc-space-md);
    }

    .section-label {
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-semibold, 600);
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--sc-accent-text, var(--sc-accent));
      margin-bottom: var(--sc-space-sm);
    }

    .channels-with-chart {
      display: flex;
      gap: var(--sc-space-lg);
      align-items: flex-start;
    }

    .channels-with-chart sc-chart {
      flex-shrink: 0;
    }

    .channels-inner {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(15rem, 1fr));
      gap: var(--sc-space-sm);
      flex: 1;
      min-width: 0;
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

    .session-row {
      cursor: pointer;
      transition: background-color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .session-row:hover {
      background-color: var(--sc-bg-elevated);
    }

    .session-row:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: -2px;
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

    .skeleton-bento {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      grid-template-areas:
        "activity activity channels channels"
        "activity activity sessions sessions";
      gap: var(--sc-space-xl);
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

    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
      .bento {
        grid-template-columns: 1fr;
        grid-template-areas:
          "activity"
          "channels"
          "sessions";
      }
      .skeleton-metrics {
        grid-template-columns: 1fr 1fr;
      }
      .skeleton-bento {
        grid-template-columns: 1fr;
        grid-template-areas:
          "activity"
          "channels"
          "sessions";
      }
      .channels-with-chart {
        flex-direction: column;
      }
    }

    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      :host {
        padding: var(--sc-space-md) var(--sc-space-lg);
      }
      .stats-row {
        grid-template-columns: 1fr;
      }
      .skeleton-metrics {
        grid-template-columns: 1fr;
      }
    }

    /* ── Quick actions row ───────────────────────────────── */

    .quick-actions {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--sc-space-lg);
      margin-bottom: var(--sc-space-2xl);
    }
    .quick-action-card {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-lg);
    }
    .quick-action-card .icon-wrap {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 2.5rem;
      height: 2.5rem;
      color: var(--sc-accent);
    }
    .quick-action-card .icon-wrap svg {
      width: 1.5rem;
      height: 1.5rem;
    }
    .quick-action-card .label {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .quick-actions {
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

  private get _channelDoughnutData() {
    const configured = this.channels.filter((c) => c.configured).length;
    const unconfigured = this.channels.length - configured;
    return {
      labels: ["Configured", "Unconfigured"],
      datasets: [{ data: [configured, unconfigured] }],
    };
  }

  private get _activitySparklineData() {
    const now = Date.now();
    const hourMs = 60 * 60 * 1000;
    const buckets: number[] = [];
    const labels: string[] = [];
    for (let i = 11; i >= 0; i--) {
      const bucketStart = now - (i + 1) * hourMs;
      const bucketEnd = now - i * hourMs;
      const count = this.activityEvents.filter((ev) => {
        const t = typeof ev.time === "number" ? ev.time : Date.now();
        return t >= bucketStart && t < bucketEnd;
      }).length;
      buckets.push(count);
      labels.push(i === 0 ? "Now" : `${i}h`);
    }
    return {
      labels,
      datasets: [{ data: buckets, color: "var(--sc-chart-brand)" }],
    };
  }

  private get _timelineItems() {
    type Ev = ActivityEvent & { message?: string; text?: string; level?: string; detail?: string };
    return this.activityEvents.map((ev: Ev) => {
      let message = ev.message ?? ev.text ?? "";
      if (!message && ev.type === "message") {
        message = `${ev.user ?? ""} via ${ev.channel ?? ""}: ${ev.preview ?? ""}`.trim();
      } else if (!message && ev.type === "tool_exec") {
        message = `Tool ${ev.tool ?? ""}: ${ev.command ?? ""}`.trim();
      } else if (!message && ev.type === "session_start") {
        message = `Session ${ev.session ?? ""} started`.trim();
      }
      if (!message) message = "Activity";
      const ts = typeof ev.time === "number" ? ev.time : Date.now();
      return {
        time: formatRelative(ts),
        message,
        status: (ev.level === "error" ? "error" : ev.level === "success" ? "success" : "info") as
          | "success"
          | "error"
          | "info"
          | "pending",
        detail: ev.detail,
      };
    });
  }

  private get _onboarded(): boolean {
    return localStorage.getItem("sc-onboarded") === "true";
  }

  /* ── Render: top-level ──────────────────────────────── */

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      ${this.error
        ? html`<sc-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <sc-button variant="primary" @click=${() => this.load()}> Retry </sc-button>
          </sc-empty-state>`
        : nothing}
      ${this._renderHero()} ${this._renderMetrics()} ${this._renderQuickActions()}
      ${this._renderDetails()}
    `;
  }

  /* ── Hero zone ──────────────────────────────────────── */

  private _renderHero() {
    if (!this._onboarded) {
      return html`
        <sc-page-hero>
          <sc-welcome-card></sc-welcome-card>
          <sc-welcome></sc-welcome>
        </sc-page-hero>
      `;
    }

    const gwOk = this.gatewayOperational;
    const cap = this.capabilities;

    return html`
      <sc-page-hero>
        <sc-section-header heading="Overview" description="Your AI assistant at a glance">
          <div class="hero-actions">
            ${this.lastLoadedAt
              ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>`
              : nothing}
            <sc-tooltip text="Reload all dashboard data" position="bottom">
              <sc-button variant="secondary" @click=${() => this.load()}>Refresh</sc-button>
            </sc-tooltip>
          </div>
        </sc-section-header>
        <div class="hero-inner">
          <div class="hero-left">
            <sc-tooltip text=${gwOk ? "All subsystems responding" : "Gateway is unreachable"}>
              <span
                class="status-dot ${gwOk ? "operational" : "offline"}"
                aria-hidden="true"
              ></span>
            </sc-tooltip>
            <div class="hero-status">
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
        </div>
      </sc-page-hero>
    `;
  }

  /* ── Stats row ─────────────────────────────────────── */

  private _renderMetrics() {
    const cap = this.capabilities;
    const uptimeValue =
      this.health.uptime_secs != null && this.health.uptime_secs > 0
        ? formatUptime(this.health.uptime_secs)
        : this.gatewayOperational
          ? "24/7"
          : "-";
    const rssValue = cap.peak_rss_mb != null ? `${cap.peak_rss_mb.toFixed(1)} MB` : "5.9 MB";
    const metrics: Array<{ label: string; value?: number; valueStr?: string }> = [
      { label: "Channels", value: cap.channels ?? 0 },
      { label: "Tools", value: cap.tools ?? 0 },
      { label: "Uptime", valueStr: uptimeValue },
      { label: "Peak RSS", valueStr: rssValue },
    ];

    return html`
      <div class="metrics-block">
        <div class="stats-row">
          ${metrics.map(
            (m, i) => html`
              <sc-stat-card
                .value=${m.value ?? 0}
                .valueStr=${m.valueStr ?? ""}
                .label=${m.label}
                style="--sc-stagger-delay: ${i * 50}ms"
              ></sc-stat-card>
            `,
          )}
        </div>
        <sc-metric-row
          .items=${[
            { label: "Sessions Today", value: String(this.sessions.length) },
            {
              label: "Channels Active",
              value: String(this.channels.filter((c) => c.configured).length),
            },
            {
              label: "Status",
              value: this.gatewayOperational ? "Healthy" : "Offline",
              accent: this.gatewayOperational ? "success" : "error",
            },
          ]}
        ></sc-metric-row>
      </div>
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
            <sc-card
              glass
              clickable
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
        <div class="bento">
          <sc-card hoverable accent class="activity">
            <div class="section-label">Live Activity</div>
            ${this.activityEvents.length > 0
              ? html`
                  <div class="activity-sparkline">
                    <sc-chart
                      type="line"
                      .data=${this._activitySparklineData}
                      height=${48}
                    ></sc-chart>
                  </div>
                `
              : nothing}
            <sc-timeline .items=${this._timelineItems}></sc-timeline>
          </sc-card>

          <sc-card hoverable accent class="channels">
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
                  <div class="channels-with-chart">
                    <sc-chart
                      type="doughnut"
                      .data=${this._channelDoughnutData}
                      height=${100}
                    ></sc-chart>
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
                  </div>
                `}
          </sc-card>

          <sc-card hoverable accent class="sessions">
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
                          <tr
                            class="session-row"
                            role="button"
                            tabindex="0"
                            aria-label=${`Open session ${s.label ?? s.key ?? "unnamed"}`}
                            @click=${() =>
                              this.dispatchEvent(
                                new CustomEvent("navigate", {
                                  detail: "chat:" + (s.key ?? ""),
                                  bubbles: true,
                                  composed: true,
                                }),
                              )}
                            @keydown=${(e: KeyboardEvent) => {
                              if (e.key === "Enter" || e.key === " ") {
                                e.preventDefault();
                                this.dispatchEvent(
                                  new CustomEvent("navigate", {
                                    detail: "chat:" + (s.key ?? ""),
                                    bubbles: true,
                                    composed: true,
                                  }),
                                );
                              }
                            }}
                          >
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
      <div class="skeleton-bento">
        <sc-skeleton variant="card" height="280px" class="activity"></sc-skeleton>
        <sc-skeleton variant="card" height="140px" class="channels"></sc-skeleton>
        <sc-skeleton variant="card" height="140px" class="sessions"></sc-skeleton>
      </div>
    `;
  }
}
