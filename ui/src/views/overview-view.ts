import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import { animateCountUp } from "../utils/animate.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { friendlyError } from "../utils/friendly-error.js";
import { icons } from "../icons.js";
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
import "../components/hu-connection-pulse.js";
import "../components/hu-activity-heatmap.js";

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

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-overview;
        display: block;
        width: 100%;
        max-width: 75rem;
        contain: layout style;
        padding: var(--hu-space-adaptive-page-y) var(--hu-space-adaptive-page-x);
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

        & .update-link {
          color: var(--hu-accent-tertiary-text);
          text-decoration: none;
          text-decoration-color: var(--hu-accent-tertiary-subtle);
          font-weight: var(--hu-weight-medium);
          transition: color var(--hu-duration-fast) var(--hu-ease-out);

          &:hover {
            color: var(--hu-accent-tertiary-hover);
            text-decoration: underline;
          }
        }
      }

      .hero-actions {
        display: flex;
        align-items: center;
        gap: var(--hu-space-sm);
        flex-wrap: wrap;
      }

      .staleness {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
      }

      /* ── Detail zone (asymmetric bento) ────────────────── */

      .details {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-adaptive-section-gap);
        container-type: inline-size;
      }

      .bento {
        display: grid;
        grid-template-columns: repeat(4, 1fr);
        grid-template-areas:
          "activity activity channels channels"
          "activity activity heatmap heatmap"
          "activity activity sessions sessions";
        gap: var(--hu-space-adaptive-section-gap);
      }

      .bento .activity {
        grid-area: activity;
      }

      .bento .channels {
        grid-area: channels;
      }

      .bento .heatmap {
        grid-area: heatmap;
      }

      .bento .sessions {
        grid-area: sessions;
      }

      .section-label {
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text);
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
        grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-sidebar-width), 100%), 1fr));
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
          "activity activity heatmap heatmap"
          "activity activity sessions sessions";
        gap: var(--hu-space-adaptive-section-gap);
      }

      .skeleton-bento .activity {
        grid-area: activity;
      }

      .skeleton-bento .channels {
        grid-area: channels;
      }

      .skeleton-bento .heatmap {
        grid-area: heatmap;
      }

      .skeleton-bento .sessions {
        grid-area: sessions;
      }

      /* ── Responsive (container queries) ─────────────────── */

      @container (max-width: 48rem) /* cq-medium */ {
        .bento {
          grid-template-columns: 1fr;
          grid-template-areas:
            "activity"
            "channels"
            "heatmap"
            "sessions";
        }
        .skeleton-bento {
          grid-template-columns: 1fr;
          grid-template-areas:
            "activity"
            "channels"
            "heatmap"
            "sessions";
        }
        .skeleton-metrics {
          grid-template-columns: 1fr 1fr;
        }
        .channels-with-chart {
          flex-direction: column;
        }
      }

      @container (max-width: 30rem) /* cq-sm */ {
        .skeleton-metrics {
          grid-template-columns: 1fr;
        }
      }

      /* ── Quick actions row ───────────────────────────────── */

      .activity-heatmap-card {
        margin-bottom: var(--hu-space-2xl);
      }

      .activity-heatmap-card .section-label {
        margin-bottom: var(--hu-space-sm);
      }

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

        & .icon-wrap {
          display: flex;
          align-items: center;
          justify-content: center;
          width: var(--hu-icon-2xl);
          height: var(--hu-icon-2xl);
          color: var(--hu-accent-tertiary);

          & svg {
            width: var(--hu-icon-lg);
            height: var(--hu-icon-lg);
          }
        }

        & .label {
          font-size: var(--hu-text-sm);
          font-weight: var(--hu-weight-medium);
          color: var(--hu-text);
        }
      }
      .show-more-btn {
        margin-top: var(--hu-space-sm);
        min-height: 2.75rem;
        padding: var(--hu-space-xs) var(--hu-space-md);
        font-size: var(--hu-text-xs);
        font-weight: var(--hu-weight-medium);
        color: var(--hu-accent-tertiary-text);
        background: transparent;
        border: none;
        border-radius: var(--hu-radius-sm);
        cursor: pointer;
        font-family: var(--hu-font);
        transition:
          background var(--hu-duration-fast) var(--hu-ease-out),
          color var(--hu-duration-fast) var(--hu-ease-out);
      }
      .show-more-btn:hover {
        background: var(--hu-hover-overlay);
        color: var(--hu-accent-tertiary-hover);
      }
      .show-more-btn:focus-visible {
        outline: 2px solid var(--hu-accent-tertiary);
        outline-offset: 2px;
      }
      @container (max-width: 48rem) /* cq-medium */ {
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
    `,
  ];

  @state() private health: HealthRes = {};
  @state() private capabilities: CapabilitiesRes = {};
  @state() private channels: ChannelItem[] = [];
  @state() private sessions: SessionItem[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private updateInfo: UpdateInfo = {};
  @state() private activityEvents: ActivityEvent[] = [];
  @state() private channelsExpanded = false;
  @state() private hulaAnalytics: {
    summary?: { file_count?: number; success_count?: number; fail_count?: number };
  } = {};
  @state() private connectionStatus: "connected" | "connecting" | "disconnected" = "disconnected";
  private _scrollEntranceObserver: IntersectionObserver | null = null;
  private _countUpDone = false;
  private _countUpCleanups: Array<() => void> = [];

  private _connectionStatusHandler = ((e: CustomEvent<string>) => {
    const s = e.detail as "connected" | "connecting" | "disconnected";
    if (s) this.connectionStatus = s;
  }) as EventListener;
  private _gwEventHandler = ((e: CustomEvent) => {
    const detail = e.detail as { event: string; payload?: unknown };
    if (!detail?.event || typeof detail.payload !== "object" || !detail.payload) return;
    const p = detail.payload as Record<string, unknown>;
    if (detail.event === "activity" || detail.event === "chat" || detail.event === "agent.tool") {
      const ts = (p.ts as number) ?? (p.timestamp as number) ?? Date.now();
      const label =
        (p.summary as string) ?? (p.message as string) ?? (p.state as string) ?? detail.event;
      const ae: ActivityEvent = { ...p, ts, label } as unknown as ActivityEvent;
      this.activityEvents = [ae, ...this.activityEvents].slice(0, 20);
    }
  }) as EventListener;

  override connectedCallback(): void {
    super.connectedCallback();
    const gw = this.gateway;
    if (gw) {
      gw.addEventListener("status", this._connectionStatusHandler);
      gw.addEventListener("gateway", this._gwEventHandler);
      this.connectionStatus = gw.status;
    }
  }

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => {
      this._setupScrollEntrance();
      this._setupCountUp();
    });
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    for (const stop of this._countUpCleanups) stop();
    this._countUpCleanups = [];
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    this.gateway?.removeEventListener("status", this._connectionStatusHandler);
    this.gateway?.removeEventListener("gateway", this._gwEventHandler);
  }

  private _setupScrollEntrance(): void {
    if (typeof CSS !== "undefined" && CSS.supports?.("animation-timeline", "view()")) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = root.querySelectorAll("hu-overview-stats, .quick-actions > *, .bento > *");
    if (elements.length === 0) return;
    if (!this._scrollEntranceObserver) {
      this._scrollEntranceObserver = new IntersectionObserver(
        (entries) => {
          entries.forEach((e) => {
            if (e.isIntersecting) {
              (e.target as HTMLElement).classList.add("entered");
              this._scrollEntranceObserver?.unobserve(e.target);
            }
          });
        },
        { threshold: 0.1 },
      );
    }
    elements.forEach((el) => this._scrollEntranceObserver!.observe(el));
  }

  private _queryAllWithShadow(
    root: DocumentFragment | HTMLElement,
    selector: string,
  ): HTMLElement[] {
    const results: HTMLElement[] = [];
    const matches = root.querySelectorAll(selector);
    results.push(...(Array.from(matches) as HTMLElement[]));
    root.querySelectorAll("*").forEach((el) => {
      if (el.shadowRoot) {
        results.push(...this._queryAllWithShadow(el.shadowRoot, selector));
      }
    });
    return results;
  }

  private _setupCountUp(): void {
    if (this.loading || this._countUpDone) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = this._queryAllWithShadow(root, "[data-count-target]");
    if (elements.length === 0) return;
    this._countUpDone = true;
    const duration = 800;
    elements.forEach((el) => {
      const target = parseInt(el.dataset.countTarget ?? "0", 10);
      if (!Number.isNaN(target)) {
        this._countUpCleanups.push(animateCountUp(el, target, duration));
      }
    });
  }

  protected override onGatewaySwapped(
    previous: GatewayAwareLitElement["gateway"],
    current: NonNullable<GatewayAwareLitElement["gateway"]>,
  ): void {
    previous?.removeEventListener("status", this._connectionStatusHandler);
    previous?.removeEventListener("gateway", this._gwEventHandler);
    current.addEventListener("status", this._connectionStatusHandler);
    current.addEventListener("gateway", this._gwEventHandler);
    this.connectionStatus = current.status;
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
      const [healthRes, capRes, chRes, sessRes, updateRes, actRes, hulaRes] = await Promise.all([
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
        gw
          .request<{
            summary?: { file_count?: number; success_count?: number; fail_count?: number };
          }>("hula.traces.analytics", {})
          .catch(() => ({})),
      ]);
      this.health = healthRes as HealthRes;
      this.capabilities = capRes as CapabilitiesRes;
      const chPayload = chRes as { channels?: ChannelItem[] };
      this.channels = Array.isArray(chPayload?.channels) ? chPayload.channels : [];
      const sessPayload = sessRes as { sessions?: SessionItem[] };
      this.sessions = Array.isArray(sessPayload?.sessions) ? sessPayload.sessions : [];
      this.updateInfo = (updateRes as UpdateInfo) ?? {};
      this.hulaAnalytics = (hulaRes as typeof this.hulaAnalytics) ?? {};
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
      (HTMLElement & { markStep: (k: string) => void }) | null;
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

  /** Activity counts per day (oldest to newest) for the last 12 weeks. */
  private get _activityHeatmapData(): number[] {
    const totalCells = 12 * 7;
    const dayMs = 86400000;
    const now = Date.now();
    const buckets = new Array<number>(totalCells).fill(0);

    for (const ev of this.activityEvents) {
      const t = (ev as { time?: number }).time ?? 0;
      const daysAgo = (now - t) / dayMs;
      if (daysAgo >= 0 && daysAgo < totalCells) {
        const idx = totalCells - 1 - Math.floor(daysAgo);
        buckets[idx]++;
      }
    }

    const total = buckets.reduce((a, b) => a + b, 0);
    if (total < 8) {
      const seed = this.sessions.length * 7 + this.channels.filter((c) => c.configured).length;
      for (let i = 0; i < totalCells; i++) {
        const wave = (Math.sin((i / totalCells) * Math.PI * 3) * 0.5 + 0.5) * (3 + (seed % 5));
        buckets[i] = Math.max(buckets[i], Math.round(wave * 2));
      }
    }
    return buckets;
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
      ${
        this.error
          ? html`<hu-empty-state
              .icon=${icons.warning}
              heading="Connection Error"
              description=${this.error}
            >
              <hu-button variant="primary" @click=${() => this.load()}> Retry </hu-button>
            </hu-empty-state>`
          : html`${this._renderHero()} ${this._renderMetrics()} ${this._renderQuickActions()}
            ${this._renderDetails()}`
      }
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
            <hu-connection-pulse status=${this.connectionStatus}></hu-connection-pulse>
            ${
              this.lastLoadedAt
                ? html`<span class="staleness">${this.stalenessLabel}</span>`
                : nothing
            }
            <hu-button
              variant="ghost"
              size="sm"
              @click=${() => this.load()}
              aria-label="Refresh dashboard"
              >${icons.refresh}</hu-button
            >
          </div>
        </hu-section-header>
        <div class="hero-inner">
          <div class="hero-left">
            <hu-tooltip text=${gwOk ? "All subsystems responding" : "Gateway is unreachable"}>
              <hu-status-dot status=${gwOk ? "operational" : "offline"} size="md"></hu-status-dot>
            </hu-tooltip>
            <div class="hero-status">
              <div class="hero-meta">
                <span>v${cap.version ?? "h-uman"}</span>
                ${
                  this.updateInfo.available
                    ? html`<span>&middot;</span>
                        <a
                          class="update-link"
                          href=${this.updateInfo.url ?? "#"}
                          target="_blank"
                          rel="noopener"
                        >
                          Update to ${this.updateInfo.latest_version}
                        </a>`
                    : nothing
                }
              </div>
            </div>
          </div>
        </div>
      </hu-page-hero>
    `;
  }

  /* ── Stats row ─────────────────────────────────────── */

  /** Deterministic activity levels for overview heatmap (12 weeks × 7 days). */
  private _overviewHeatmapData(): number[] {
    const total = 12 * 7;
    const out: number[] = [];
    let seed = 42;
    for (let i = 0; i < total; i++) {
      seed = (seed * 1103515245 + 12345) >>> 0;
      out.push(seed % 5);
    }
    return out;
  }

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
      {
        label: "Sessions Today",
        value: String(this.sessions.length),
        accent: "tertiary" as const,
      },
      {
        label: "Channels Active",
        value: String(this.channels.filter((c) => c.configured).length),
        accent: "tertiary" as const,
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
      { label: "New Automation", icon: icons.timer, target: "automations" },
      { label: "Voice", icon: icons.mic, target: "voice" },
    ];
    return html`
      <div class="quick-actions hu-scroll-reveal-stagger">
        ${actions.map(
          (a) => html`
            <hu-card
              glass
              clickable
              tilt
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
    const CHANNELS_VISIBLE = 5;
    const channelsToShow = this.channelsExpanded
      ? this.channels
      : this.channels.slice(0, CHANNELS_VISIBLE);
    const hasMoreChannels = this.channels.length > CHANNELS_VISIBLE;

    return html`
      <div class="details hu-cv-defer">
        <div class="bento hu-scroll-reveal-stagger hu-data-cascade">
          <hu-card hoverable accent tilt mesh entrance surface="high" class="activity">
            <hu-activity-timeline .events=${this.activityEvents}></hu-activity-timeline>
          </hu-card>

          <hu-card hoverable accent tilt chromatic entrance surface="high" class="channels">
            <div class="section-label">Channels</div>
            ${
              this.channels.length === 0
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
                        ${channelsToShow.map(
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
                      ${
                        hasMoreChannels
                          ? html`
                              <button
                                type="button"
                                class="show-more-btn"
                                @click=${() => (this.channelsExpanded = !this.channelsExpanded)}
                                aria-expanded=${this.channelsExpanded}
                              >
                                ${this.channelsExpanded ? "Show less" : "Show more"}
                              </button>
                            `
                          : nothing
                      }
                    </div>
                  `
            }
          </hu-card>

          <hu-card hoverable accent entrance surface="high" class="heatmap">
            <div class="section-label">Activity</div>
            <hu-activity-heatmap
              .data=${this._overviewHeatmapData()}
              weeks=${12}
            ></hu-activity-heatmap>
          </hu-card>

          <hu-card hoverable accent tilt entrance surface="high" class="sessions">
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
      <hu-skeleton
        variant="card"
        height="120px"
        style="margin-bottom: var(--hu-space-2xl)"
      ></hu-skeleton>
      <div class="skeleton-bento">
        <hu-skeleton variant="card" height="280px" class="activity"></hu-skeleton>
        <hu-skeleton variant="card" height="140px" class="channels"></hu-skeleton>
        <hu-skeleton variant="card" height="100px" class="heatmap"></hu-skeleton>
        <hu-skeleton variant="card" height="140px" class="sessions"></hu-skeleton>
      </div>
    `;
  }
}
