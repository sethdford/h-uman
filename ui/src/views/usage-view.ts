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
import "../components/sc-animated-number.js";

interface UsageSummary {
  session_cost_usd?: number;
  daily_cost_usd?: number;
  monthly_cost_usd?: number;
  total_tokens?: number;
  request_count?: number;
}

@customElement("sc-usage-view")
export class ScUsageView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 960px;
    }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    @media (max-width: 640px) {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
    .cards {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
    }
    .card-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }
    .card-value {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      font-variant-numeric: tabular-nums;
    }
    .chart-section {
      margin-top: var(--sc-space-2xl);
    }
    .chart-title {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-sm);
    }
    .bar-chart {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .bar-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .bar-label {
      width: 7.5rem;
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      flex-shrink: 0;
    }
    .bar-value {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
      min-width: 4rem;
    }
    .bar-track {
      flex: 1;
      height: var(--sc-space-lg, 1.25rem);
      background: var(--sc-bg-elevated);
      border-radius: var(--sc-radius-sm);
      overflow: hidden;
    }
    .bar-fill {
      height: 100%;
      width: var(--bar-pct, 0%);
      background: var(--sc-accent);
      border-radius: var(--sc-radius-sm);
      transition: width var(--sc-duration-normal) var(--sc-ease-out);
    }
    @media (max-width: 768px) {
      .cards {
        grid-template-columns: 1fr 1fr;
      }
      .bar-row {
        flex-wrap: wrap;
      }
      .bar-label {
        width: 100%;
      }
    }
    @media (max-width: 480px) {
      .cards {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .bar-fill {
        transition: none;
      }
    }
  `;

  @state() private summary: UsageSummary = {};
  @state() private loading = false;
  @state() private error = "";

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

  private barPct(value: number, max: number): number {
    if (max <= 0) return 0;
    return Math.min(100, (value / max) * 100);
  }

  private _renderSkeleton() {
    return html`
      <div class="cards sc-stagger">
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
      </div>
    `;
  }

  private _renderStats(
    sessionCost: number,
    dailyCost: number,
    monthlyCost: number,
    totalTokens: number,
    requestCount: number,
  ) {
    return html`
      <div class="cards sc-stagger" aria-live="polite">
        <sc-card>
          <div class="card-label">Session Cost</div>
          <div class="card-value">
            <sc-animated-number .value=${sessionCost} prefix="$"></sc-animated-number>
          </div>
        </sc-card>
        <sc-card>
          <div class="card-label">Daily Cost</div>
          <div class="card-value">
            <sc-animated-number .value=${dailyCost} prefix="$"></sc-animated-number>
          </div>
        </sc-card>
        <sc-card>
          <div class="card-label">Monthly Cost</div>
          <div class="card-value">
            <sc-animated-number .value=${monthlyCost} prefix="$"></sc-animated-number>
          </div>
        </sc-card>
        <sc-card>
          <div class="card-label">Total Tokens</div>
          <div class="card-value">
            <sc-animated-number .value=${totalTokens}></sc-animated-number>
          </div>
        </sc-card>
        <sc-card>
          <div class="card-label">Request Count</div>
          <div class="card-value">
            <sc-animated-number .value=${requestCount}></sc-animated-number>
          </div>
        </sc-card>
      </div>
    `;
  }

  private _renderChart(
    sessionCost: number,
    dailyCost: number,
    monthlyCost: number,
    maxCost: number,
  ) {
    return html`
      <div class="chart-section">
        <div class="chart-title">Cost comparison (bar chart)</div>
        <div class="bar-chart">
          <div class="bar-row">
            <span class="bar-label">Session</span>
            <div class="bar-track">
              <div class="bar-fill" style="--bar-pct: ${this.barPct(sessionCost, maxCost)}%"></div>
            </div>
            <span class="bar-value">${this.formatCurrency(sessionCost)}</span>
          </div>
          <div class="bar-row">
            <span class="bar-label">Daily</span>
            <div class="bar-track">
              <div class="bar-fill" style="--bar-pct: ${this.barPct(dailyCost, maxCost)}%"></div>
            </div>
            <span class="bar-value">${this.formatCurrency(dailyCost)}</span>
          </div>
          <div class="bar-row">
            <span class="bar-label">Monthly</span>
            <div class="bar-track">
              <div class="bar-fill" style="--bar-pct: ${this.barPct(monthlyCost, maxCost)}%"></div>
            </div>
            <span class="bar-value">${this.formatCurrency(monthlyCost)}</span>
          </div>
        </div>
      </div>
    `;
  }

  override render() {
    const s = this.summary;
    const sessionCost = s.session_cost_usd ?? 0;
    const dailyCost = s.daily_cost_usd ?? 0;
    const monthlyCost = s.monthly_cost_usd ?? 0;
    const totalTokens = s.total_tokens ?? 0;
    const requestCount = s.request_count ?? 0;
    const maxCost = Math.max(sessionCost, dailyCost, monthlyCost, 0.01);

    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Usage"
          description="Token consumption, cost tracking, and request metrics"
        >
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
          style="--sc-stagger-delay: 80ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${requestCount}
          label="Requests"
          style="--sc-stagger-delay: 160ms"
        ></sc-stat-card>
      </div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading
        ? this._renderSkeleton()
        : sessionCost === 0 &&
            dailyCost === 0 &&
            monthlyCost === 0 &&
            totalTokens === 0 &&
            requestCount === 0
          ? html`
              <sc-empty-state
                .icon=${icons["bar-chart"]}
                heading="No usage data"
                description="Usage metrics will appear here once you start making requests."
              ></sc-empty-state>
            `
          : html`
              ${this._renderStats(sessionCost, dailyCost, monthlyCost, totalTokens, requestCount)}
              ${this._renderChart(sessionCost, dailyCost, monthlyCost, maxCost)}
            `}
    `;
  }
}
