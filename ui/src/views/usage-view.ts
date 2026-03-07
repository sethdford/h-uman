import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-stat-card.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";
import "../components/sc-chart.js";
import type { ChartData } from "../components/sc-chart.js";
import "../components/sc-segmented-control.js";

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
  "var(--sc-chart-categorical-1)",
  "var(--sc-chart-categorical-2)",
  "var(--sc-chart-categorical-3)",
  "var(--sc-chart-categorical-4)",
  "var(--sc-chart-categorical-5)",
];

const TIME_RANGE_OPTIONS = [
  { value: "24h", label: "24h" },
  { value: "7d", label: "7d" },
  { value: "30d", label: "30d" },
];

@customElement("sc-usage-view")
export class ScUsageView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-usage;
      display: block;
      color: var(--sc-text);
      max-width: 60rem;
    }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
    }
    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
    .section {
      margin-bottom: var(--sc-space-2xl);
    }
    .section-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
      margin-bottom: var(--sc-space-sm);
    }
    .chart-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-md);
      flex-wrap: wrap;
    }
    .chart-inner {
      padding: var(--sc-space-sm) var(--sc-space-md) var(--sc-space-md);
    }
    .provider-list {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
    }
    .provider-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .provider-dot {
      width: 0.625rem;
      height: 0.625rem;
      border-radius: var(--sc-radius-full);
      flex-shrink: 0;
    }
    .provider-name {
      width: 6.5rem;
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      flex-shrink: 0;
      font-weight: var(--sc-weight-medium);
    }
    .provider-bar-track {
      flex: 1;
      height: var(--sc-space-md);
      background: var(--sc-bg-inset);
      border-radius: var(--sc-radius-sm);
      overflow: hidden;
    }
    .provider-bar-fill {
      height: 100%;
      border-radius: var(--sc-radius-sm);
      transition: width var(--sc-duration-normal) var(--sc-ease-out);
    }
    .provider-cost {
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      font-variant-numeric: tabular-nums;
      font-weight: var(--sc-weight-medium);
      min-width: 4rem;
      text-align: right;
    }
    .provider-pct {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      min-width: 2.5rem;
      text-align: right;
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .provider-name {
        width: 5rem;
      }
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
      this.error = e instanceof Error ? e.message : "Failed to load usage";
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
          color: "var(--sc-chart-brand)",
          backgroundColor: "var(--sc-chart-brand)",
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
      <div class="stats-row">
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
      </div>
      <sc-skeleton variant="card" height="200px"></sc-skeleton>
      <sc-skeleton variant="card" height="200px"></sc-skeleton>
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
          <sc-segmented-control
            .options=${TIME_RANGE_OPTIONS}
            .value=${this._timeRange}
            @sc-change=${(e: CustomEvent<{ value: string }>) => {
              this._timeRange = e.detail.value as "24h" | "7d" | "30d";
            }}
          ></sc-segmented-control>
        </div>
        <sc-card glass>
          <div class="chart-inner">
            <sc-chart type="area" .data=${chartData} height=${200}></sc-chart>
          </div>
        </sc-card>
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
        <sc-card glass>
          <div class="chart-inner">
            <sc-chart type="bar" .data=${chartData} height=${140} horizontal></sc-chart>
          </div>
        </sc-card>
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
        <sc-card glass>
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
        </sc-card>
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
      <sc-page-hero>
        <sc-section-header
          heading="Usage"
          description="Token consumption, cost tracking, and forecasting"
        >
          <sc-button
            variant="ghost"
            size="sm"
            @click=${this._exportUsage}
            aria-label="Export usage data as JSON"
          >
            <span style="display:inline-flex;width:1em;height:1em" aria-hidden="true"
              >${icons.export}</span
            >
            Export
          </sc-button>
        </sc-section-header>
      </sc-page-hero>

      <div class="stats-row">
        <sc-stat-card
          .value=${totalTokens}
          label="Tokens Today"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${dailyCost}
          label="Cost Today"
          prefix="$"
          style="--sc-stagger-delay: 50ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${requestCount}
          label="Requests"
          style="--sc-stagger-delay: 100ms"
        ></sc-stat-card>
      </div>

      ${this.error
        ? html`<sc-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <sc-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry loading usage"
              >Retry</sc-button
            >
          </sc-empty-state>`
        : nothing}
      ${this.loading
        ? this._renderSkeleton()
        : isEmpty
          ? html`
              <sc-empty-state
                .icon=${icons["bar-chart"]}
                heading="No usage data"
                description="Start a conversation or run a tool to generate usage metrics. Data will appear here once requests are made."
              ></sc-empty-state>
            `
          : html`
              ${this._renderTokenChart()} ${this._renderCostBreakdownChart()}
              ${this._renderProviders()}
            `}
    `;
  }
}
