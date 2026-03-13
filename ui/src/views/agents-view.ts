import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate, formatRelative } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/hu-data-table-v2.js";
import type { ChartData } from "../components/hu-chart.js";
import "../components/hu-button.js";
import "../components/hu-card.js";
import "../components/hu-chart.js";
import "../components/hu-data-table-v2.js";
import "../components/hu-empty-state.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-skeleton.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";

function friendlyError(e: unknown): string {
  const msg = e instanceof Error ? e.message : String(e);
  if (msg.includes("timeout")) return "Request timed out. Please try again.";
  if (msg.includes("WebSocket")) return "Connection lost. Reconnecting...";
  if (msg.includes("404")) return "Resource not found.";
  if (msg.includes("401") || msg.includes("unauthorized"))
    return "Authentication failed. Please check your credentials.";
  if (msg.includes("403") || msg.includes("forbidden")) return "Access denied.";
  if (msg.includes("network")) return "Network error. Please check your connection.";
  return "Something went wrong. Please try again.";
}

interface ConfigData {
  default_provider?: string;
  default_model?: string;
  temperature?: number;
  max_tokens?: number;
}

interface Session {
  key?: string;
  label?: string;
  turn_count?: number;
  last_active?: number;
  created_at?: number;
}

interface Capabilities {
  tools?: unknown[];
  channels?: unknown[];
  providers?: unknown[];
}

function toDateKey(ts: number | undefined): string {
  if (ts == null) return "";
  const d = new Date(ts < 1e12 ? ts * 1000 : ts);
  return d.toISOString().slice(0, 10);
}

function formatChartLabel(dateKey: string): string {
  const d = new Date(dateKey + "T12:00:00");
  return d.toLocaleDateString(undefined, { month: "short", day: "numeric" });
}

@customElement("hu-agents-view")
export class ScAgentsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-agents;
      display: block;
      color: var(--hu-text);
      max-width: 75rem;
      padding: var(--hu-space-lg) var(--hu-space-xl);
    }

    .staleness {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .section-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--hu-space-sm);
    }

    .section-title {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .card-spacer {
      margin-bottom: var(--hu-space-2xl);
    }

    .chart-section {
      margin-bottom: var(--hu-space-2xl);
    }

    .profile-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--hu-space-sm);
    }

    .profile-title {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .profile-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(8.75rem, 1fr));
      gap: var(--hu-space-sm);
      font-size: var(--hu-text-sm);
    }

    .profile-item {
      color: var(--hu-text-muted);
    }

    .profile-item strong {
      color: var(--hu-text);
    }

    .skeleton-sessions {
      margin-bottom: var(--hu-space-2xl);
    }

    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .metrics,
      .skeleton-metrics {
        grid-template-columns: repeat(2, 1fr);
      }
      .profile-grid {
        grid-template-columns: 1fr 1fr;
      }
    }

    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
      .metrics,
      .skeleton-metrics {
        grid-template-columns: 1fr;
      }
      .profile-grid {
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

  @state() private config: ConfigData = {};
  @state() private sessions: Session[] = [];
  @state() private capabilities: Capabilities = {};
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const [cfg, sess, caps] = await Promise.all([
        gw.request<Partial<ConfigData>>("config.get", {}),
        gw.request<{ sessions?: Session[] }>("sessions.list", {}),
        gw.request<Capabilities>("capabilities", {}),
      ]);
      this.config = {
        default_provider: cfg?.default_provider ?? "",
        default_model: cfg?.default_model ?? "",
        temperature: cfg?.temperature ?? 0.7,
        max_tokens: cfg?.max_tokens ?? 0,
      };
      this.sessions = sess?.sessions ?? [];
      this.capabilities = {
        tools: caps?.tools ?? [],
        channels: caps?.channels ?? [],
        providers: caps?.providers ?? [],
      };
    } catch (e) {
      this.error = friendlyError(e);
    } finally {
      this.loading = false;
    }
  }

  private dispatchNavigate(tab: string): void {
    this.dispatchEvent(
      new CustomEvent("navigate", {
        detail: tab,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private get totalTurns(): number {
    return this.sessions.reduce((s, x) => s + (x.turn_count ?? 0), 0);
  }

  private get _toolCount(): number {
    const t = this.capabilities.tools;
    return typeof t === "number" ? t : Array.isArray(t) ? t.length : 0;
  }

  private get _channelCount(): number {
    const ch = this.capabilities.channels;
    return typeof ch === "number" ? ch : Array.isArray(ch) ? ch.length : 0;
  }

  private get tableRows(): Record<string, unknown>[] {
    return this.sessions.map((s) => ({
      label: s.label ?? s.key ?? "—",
      created: s.created_at,
      lastActive: s.last_active,
      turns: s.turn_count ?? 0,
      _sessionKey: s.key ?? "default",
    }));
  }

  private readonly columns: DataTableColumnV2[] = [
    { key: "label", label: "Label", sortable: true },
    {
      key: "created",
      label: "Created",
      sortable: true,
      render: (v) => formatDate(v as number | undefined),
    },
    {
      key: "lastActive",
      label: "Last Active",
      sortable: true,
      render: (v) => formatRelative(v as number | undefined),
    },
    {
      key: "turns",
      label: "Turns",
      sortable: true,
      render: (v) => String(v ?? 0),
    },
  ];

  private get sessionsPerDayChart(): ChartData {
    const counts = new Map<string, number>();
    for (const s of this.sessions) {
      const key = toDateKey(s.created_at);
      if (key) counts.set(key, (counts.get(key) ?? 0) + 1);
    }
    const sorted = [...counts.entries()].sort((a, b) => a[0].localeCompare(b[0]));
    return {
      labels: sorted.map(([k]) => formatChartLabel(k)),
      datasets: [
        {
          label: "Sessions",
          data: sorted.map(([, v]) => v),
          backgroundColor: "var(--hu-chart-brand, var(--hu-accent))",
        },
      ],
    };
  }

  private _onRowClick(e: CustomEvent<{ row: Record<string, unknown>; index: number }>): void {
    const key = e.detail.row._sessionKey as string;
    this.dispatchNavigate("chat:" + key);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      ${this.error
        ? html`<hu-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></hu-empty-state>`
        : nothing}
      ${this._renderHero()} ${this._renderMetrics()} ${this._renderChart()}
      ${this._renderSessions()} ${this._renderConfig()}
    `;
  }

  private _renderHero() {
    return html`
      <hu-page-hero role="region" aria-label="h-uman Agent overview">
        <hu-section-header heading="h-uman Agent" description="Monitor autonomous agent instances">
          <span class="staleness">${this.stalenessLabel}</span>
          <hu-button size="sm" @click=${() => this.load()} aria-label="Refresh data"
            >Refresh</hu-button
          >
        </hu-section-header>
      </hu-page-hero>
    `;
  }

  private _renderMetrics() {
    const metrics = [
      { label: "Sessions", value: this.sessions.length },
      { label: "Turns", value: this.totalTurns },
      { label: "Tools", value: this._toolCount },
      { label: "Channels", value: this._channelCount },
    ];

    return html`
      <hu-stats-row>
        ${metrics.map(
          (m, i) => html`
            <hu-stat-card
              .value=${m.value}
              .label=${m.label}
              style="--hu-stagger-delay: ${i * 50}ms"
            ></hu-stat-card>
          `,
        )}
      </hu-stats-row>
    `;
  }

  private _renderChart() {
    const chartData = this.sessionsPerDayChart;
    if (chartData.labels.length === 0) return nothing;
    return html`
      <hu-card class="chart-section">
        <div class="section-header">
          <span class="section-title">Sessions per day</span>
        </div>
        <hu-chart type="bar" .data=${chartData} height=${200}></hu-chart>
      </hu-card>
    `;
  }

  private _renderSessions() {
    return html`
      <hu-card class="card-spacer">
        <div class="section-header">
          <span class="section-title">Active Sessions</span>
          <hu-button
            variant="primary"
            size="sm"
            @click=${() => this.dispatchNavigate("chat:default")}
            aria-label="Start new conversation"
          >
            + New Chat
          </hu-button>
        </div>
        ${this.sessions.length === 0
          ? html`
              <hu-empty-state
                .icon=${icons["chat-circle"]}
                heading="No active sessions"
                description="Start a conversation to see sessions here."
              ></hu-empty-state>
            `
          : html`
              <hu-data-table-v2
                .columns=${this.columns}
                .rows=${this.tableRows}
                @hu-row-click=${this._onRowClick}
              ></hu-data-table-v2>
            `}
      </hu-card>
    `;
  }

  private _renderConfig() {
    return html`
      <hu-card>
        <div class="profile-header">
          <span class="profile-title">Configuration</span>
          <hu-button
            size="sm"
            @click=${() => this.dispatchNavigate("config")}
            aria-label="Edit configuration"
          >
            Edit Config
          </hu-button>
        </div>
        <div class="profile-grid">
          <div class="profile-item">
            Provider: <strong>${this.config.default_provider ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Model: <strong>${this.config.default_model ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Temperature: <strong>${this.config.temperature ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Max tokens: <strong>${this.config.max_tokens ?? "\u2014"}</strong>
          </div>
        </div>
      </hu-card>
    `;
  }

  private _renderSkeleton() {
    return html`
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <hu-skeleton variant="card" height="160px" class="skeleton-sessions"></hu-skeleton>
      <hu-skeleton variant="card" height="100px"></hu-skeleton>
    `;
  }
}
