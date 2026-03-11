import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-card.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import "../components/hu-chart.js";
import type { ChartData } from "../components/hu-chart.js";
import "../components/hu-segmented-control.js";

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

interface DailyCost {
  date: string;
  cost: number;
}

interface ProviderUsage {
  provider: string;
  tokens: number;
  cost: number;
}

interface UsageSummary {
  session_cost_usd?: number;
  daily_cost_usd?: number;
  monthly_cost_usd?: number;
  projected_monthly_usd?: number;
  previous_month_cost_usd?: number;
  total_tokens?: number;
  request_count?: number;
  token_trend?: number[];
  daily_cost_history?: DailyCost[];
  by_provider?: ProviderUsage[];
  cost_per_request?: number;
  tokens_per_turn?: number;
  turns_today?: number;
  turns_week?: number;
  days_in_month?: number;
}

const CHART_COLORS = [
  "var(--hu-chart-categorical-1)",
  "var(--hu-chart-categorical-2)",
  "var(--hu-chart-categorical-3)",
  "var(--hu-chart-categorical-4)",
  "var(--hu-chart-categorical-5)",
];

const TIME_RANGE_OPTIONS = [
  { value: "24h", label: "24h" },
  { value: "7d", label: "7d" },
  { value: "30d", label: "30d" },
];

@customElement("hu-usage-view")
export class ScUsageView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-usage;
      display: block;
      color: var(--hu-text);
      max-width: 60rem;
    }
    .section {
      margin-bottom: var(--hu-space-2xl);
    }
    .section-label {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
      margin-bottom: var(--hu-space-sm);
    }
    .chart-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-md);
      margin-bottom: var(--hu-space-md);
      flex-wrap: wrap;
    }
    .chart-inner {
      padding: var(--hu-space-sm) var(--hu-space-md) var(--hu-space-md);
    }
    .provider-list {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }
    .provider-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }
    .provider-dot {
      width: 0.625rem;
      height: 0.625rem;
      border-radius: var(--hu-radius-full);
      flex-shrink: 0;
    }
    .provider-name {
      width: 6.5rem;
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      flex-shrink: 0;
      font-weight: var(--hu-weight-medium);
    }
    .provider-bar-track {
      flex: 1;
      height: var(--hu-space-md);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-sm);
      overflow: hidden;
    }
    .provider-bar-fill {
      height: 100%;
      border-radius: var(--hu-radius-sm);
      transition: width var(--hu-duration-normal) var(--hu-ease-out);
    }
    .provider-cost {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-variant-numeric: tabular-nums;
      font-weight: var(--hu-weight-medium);
      min-width: 4rem;
      text-align: right;
    }
    .provider-pct {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      min-width: 2.5rem;
      text-align: right;
    }
    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
      .provider-name {
        width: 5rem;
      }
    }
    .icon-inline {
      display: inline-flex;
      width: 1em;
      height: 1em;
    }
    @media (prefers-reduced-motion: reduce) {
      .provider-bar-fill {
        transition: none;
      }
    }
  `;

  @state() private summary: UsageSummary = {};
  @state() private loading = false;
  @state() private error = "";
  @state() private _timeRange: "24h" | "7d" | "30d" = "24h";

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  protected override async load(): Promise<void> {
    await this.loadSummary();
  }

  private async loadSummary(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const res = (await gw.request<UsageSummary>("usage.summary", {})) as
        | UsageSummary
        | { result?: UsageSummary };
      this.summary =
        (res && "result" in res && res.result) ||
        (res && "session_cost_usd" in res ? (res as UsageSummary) : {}) ||
        {};
    } catch (e) {
      this.error = friendlyError(e);
      this.summary = {};
    } finally {
      this.loading = false;
    }
  }

  private formatCurrency(v: number | undefined): string {
    if (v == null || Number.isNaN(v)) return "$0.00";
    return new Intl.NumberFormat("en-US", {
      style: "currency",
      currency: "USD",
      minimumFractionDigits: 2,
    }).format(v);
  }

  private formatNumber(v: number | undefined): string {
    if (v == null || Number.isNaN(v)) return "0";
    return new Intl.NumberFormat("en-US").format(v);
  }

  private _tokenChartLabels(): string[] {
    const count = this._timeRange === "24h" ? 24 : this._timeRange === "7d" ? 7 : 30;
    if (this._timeRange === "24h") {
      return Array.from({ length: count }, (_, i) => `${i.toString().padStart(2, "0")}:00`);
    }
    return Array.from({ length: count }, (_, i) => `Day ${i + 1}`);
  }

  private _tokenChartData(): ChartData {
    const trend = this.summary.token_trend ?? [];
    const labels = this._tokenChartLabels();
    const count = labels.length;
    const data =
      trend.length >= count
        ? trend.slice(-count)
        : [...Array(count - trend.length).fill(0), ...trend];
    return {
      labels,
      datasets: [
        {
          data,
          color: "var(--hu-chart-brand)",
          backgroundColor: "var(--hu-chart-brand)",
        },
      ],
    };
  }

  private _costBreakdownData(): {
    labels: string[];
    datasets: { data: number[]; backgroundColor?: string[] }[];
  } {
    const s = this.summary;
    const sessionCost = s.session_cost_usd ?? 0;
    const dailyCost = s.daily_cost_usd ?? 0;
    const monthlyCost = s.monthly_cost_usd ?? 0;
    return {
      labels: ["Session", "Daily", "Monthly"],
      datasets: [
        {
          data: [sessionCost, dailyCost, monthlyCost],
          backgroundColor: [CHART_COLORS[0], CHART_COLORS[1], CHART_COLORS[2]],
        },
      ],
    };
  }

  private _exportUsage(): void {
    const blob = new Blob([JSON.stringify(this.summary, null, 2)], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `usage-${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }

  private _renderSkeleton() {
    return html`
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <hu-skeleton variant="card" height="200px"></hu-skeleton>
      <hu-skeleton variant="card" height="200px"></hu-skeleton>
    `;
  }

  private _renderTokenChart() {
    const trend = this.summary.token_trend;
    if (!trend || trend.length < 2) return nothing;

    const chartData = this._tokenChartData();
    return html`
      <div class="section" role="region" aria-label="Token usage over time">
        <div class="chart-header">
          <div class="section-label">Token Usage</div>
          <hu-segmented-control
            .options=${TIME_RANGE_OPTIONS}
            .value=${this._timeRange}
            @hu-change=${(e: CustomEvent<{ value: string }>) => {
              this._timeRange = e.detail.value as "24h" | "7d" | "30d";
            }}
          ></hu-segmented-control>
        </div>
        <hu-card glass>
          <div class="chart-inner">
            <hu-chart type="area" .data=${chartData} height=${200}></hu-chart>
          </div>
        </hu-card>
      </div>
    `;
  }

  private _renderCostBreakdownChart() {
    const s = this.summary;
    const sessionCost = s.session_cost_usd ?? 0;
    const dailyCost = s.daily_cost_usd ?? 0;
    const monthlyCost = s.monthly_cost_usd ?? 0;
    const hasData = sessionCost > 0 || dailyCost > 0 || monthlyCost > 0;
    if (!hasData) return nothing;

    const chartData = this._costBreakdownData();
    return html`
      <div class="section">
        <div class="section-label">Cost Breakdown</div>
        <hu-card glass>
          <div class="chart-inner">
            <hu-chart type="bar" .data=${chartData} height=${140} horizontal></hu-chart>
          </div>
        </hu-card>
      </div>
    `;
  }

  private _renderProviders() {
    const providers = this.summary.by_provider;
    if (!providers || providers.length === 0) return nothing;

    const totalCost = providers.reduce((s, p) => s + p.cost, 0) || 1;
    const maxCost = Math.max(...providers.map((p) => p.cost), 0.01);

    return html`
      <div class="section">
        <div class="section-label">Cost by Provider</div>
        <hu-card glass>
          <div class="provider-list">
            ${providers.map(
              (p, i) => html`
                <div class="provider-row">
                  <span
                    class="provider-dot"
                    style="background: ${CHART_COLORS[i % CHART_COLORS.length]}"
                  ></span>
                  <span class="provider-name">${p.provider}</span>
                  <div class="provider-bar-track">
                    <div
                      class="provider-bar-fill"
                      style="width: ${(p.cost / maxCost) * 100}%; background: ${CHART_COLORS[
                        i % CHART_COLORS.length
                      ]}"
                    ></div>
                  </div>
                  <span class="provider-cost">${this.formatCurrency(p.cost)}</span>
                  <span class="provider-pct">${Math.round((p.cost / totalCost) * 100)}%</span>
                </div>
              `,
            )}
          </div>
        </hu-card>
      </div>
    `;
  }

  override render() {
    const s = this.summary;
    const dailyCost = s.daily_cost_usd ?? 0;
    const totalTokens = s.total_tokens ?? 0;
    const requestCount = s.request_count ?? 0;

    const isEmpty = dailyCost === 0 && totalTokens === 0 && requestCount === 0;

    return html`
      <hu-page-hero role="region" aria-label="Usage">
        <hu-section-header
          heading="Usage"
          description="Token consumption, cost tracking, and forecasting"
        >
          <hu-button
            variant="ghost"
            size="sm"
            @click=${this._exportUsage}
            aria-label="Export usage data as JSON"
          >
            <span class="icon-inline" aria-hidden="true">${icons.export}</span>
            Export
          </hu-button>
        </hu-section-header>
      </hu-page-hero>

      <hu-stats-row>
        <hu-stat-card
          .value=${totalTokens}
          label="Tokens Today"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${dailyCost}
          label="Cost Today"
          prefix="$"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${requestCount}
          label="Requests"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
      </hu-stats-row>

      ${this.error
        ? html`<hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <hu-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry loading usage"
              >Retry</hu-button
            >
          </hu-empty-state>`
        : nothing}
      ${this.loading
        ? this._renderSkeleton()
        : isEmpty
          ? html`
              <hu-empty-state
                .icon=${icons["bar-chart"]}
                heading="No usage data"
                description="Start a conversation or run a tool to generate usage metrics. Data will appear here once requests are made."
              ></hu-empty-state>
            `
          : html`
              ${this._renderTokenChart()} ${this._renderCostBreakdownChart()}
              ${this._renderProviders()}
            `}
    `;
  }
}
