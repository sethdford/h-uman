import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-search.js";
import "../components/hu-chart.js";
import "../components/hu-combobox.js";
import "../components/hu-button.js";
import type { ChartData } from "../components/hu-chart.js";
import { friendlyError } from "../utils/friendly-error.js";

interface ProviderItem {
  name?: string;
  has_key?: boolean;
  base_url?: string;
  native_tools?: boolean;
  is_default?: boolean;
}

interface ModelsListRes {
  default_model?: string;
  providers?: ProviderItem[];
}

interface ConfigGetRes {
  default_model?: string;
  default_provider?: string;
}

interface ProviderUsage {
  provider: string;
  tokens: number;
  cost: number;
}

interface UsageSummary {
  by_provider?: ProviderUsage[];
}

@customElement("hu-models-view")
export class ScModelsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-models;
      display: block;
      max-width: 75rem;
    }
    .info-section {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--hu-space-xl);
      margin-bottom: var(--hu-space-2xl);
    }
    .info-item {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }
    .chart-section {
      margin-bottom: var(--hu-space-2xl);
    }
    .chart-header {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-md);
    }
    .section-label {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(17.5rem, 1fr));
      gap: var(--hu-space-xl);
    }
    .card-header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      margin-bottom: var(--hu-space-sm);
      flex-wrap: wrap;
    }
    .card-name {
      font-weight: var(--hu-weight-semibold);
      font-size: var(--hu-text-lg);
      color: var(--hu-text);
    }
    .card-name.default {
      color: var(--hu-accent-text, var(--hu-accent));
    }
    .card-url {
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font-mono);
      color: var(--hu-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      margin-top: var(--hu-space-xs);
    }
    .key-status {
      font-size: var(--hu-text-sm);
      margin-top: var(--hu-space-sm);
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }
    .key-status.has {
      color: var(--hu-success);
    }
    .key-status.missing {
      color: var(--hu-error);
    }
    .key-icon {
      width: 0.875rem;
      height: 0.875rem;
      display: inline-block;
      vertical-align: middle;
    }
    .card-actions {
      margin-top: var(--hu-space-md);
      display: flex;
      justify-content: flex-end;
    }
    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .info-section {
        grid-template-columns: 1fr;
      }
      .grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 40rem) /* --hu-breakpoint-md */ {
      .grid {
        grid-template-columns: 1fr;
      }
    }
    .search-wrap {
      margin-bottom: var(--hu-space-xl);
    }
    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
      .grid {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
    }
  `;

  @state() private defaultModel = "";
  @state() private defaultProvider = "";
  @state() private providers: ProviderItem[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private filter = "";
  @state() private usageByProvider: ProviderUsage[] = [];
  @state() private settingDefault = false;
  @state() private _refreshing = false;

  private get filteredProviders(): ProviderItem[] {
    const q = this.filter.toLowerCase().trim();
    if (!q) return this.providers;
    return this.providers.filter((p) => (p.name ?? "").toLowerCase().includes(q));
  }

  private get providerOptions(): { value: string; label: string }[] {
    return this.providers.filter((p) => p.name).map((p) => ({ value: p.name!, label: p.name! }));
  }

  private get requestDistributionChartData(): ChartData | null {
    if (!this.usageByProvider.length) return null;
    const byProvider = this.usageByProvider.filter((p) => (p.tokens ?? 0) > 0);
    if (byProvider.length === 0) return null;
    return {
      labels: byProvider.map((p) => p.provider),
      datasets: [
        {
          data: byProvider.map((p) => p.tokens ?? 0),
          label: "Tokens",
        },
      ],
    };
  }

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.loading = false;
      return;
    }
    this._refreshing = this.providers.length > 0;
    this.loading = this.providers.length === 0;
    try {
      const [modelsRes, configRes, usageRes] = await Promise.all([
        gw.request<ModelsListRes>("models.list", {}).catch((): Partial<ModelsListRes> => ({})),
        gw.request<ConfigGetRes>("config.get", {}).catch((): Partial<ConfigGetRes> => ({})),
        gw.request<UsageSummary>("usage.summary", {}).catch((): Partial<UsageSummary> => ({})),
      ]);
      this.defaultModel = modelsRes?.default_model ?? configRes?.default_model ?? "";
      this.defaultProvider = configRes?.default_provider ?? "";
      this.providers = modelsRes?.providers ?? [];
      this.usageByProvider = usageRes?.by_provider ?? [];
    } catch (e) {
      this.providers = [];
      this.defaultModel = "";
      this.defaultProvider = "";
      this.usageByProvider = [];
      this.error = friendlyError(e);
    } finally {
      this.loading = false;
      this._refreshing = false;
    }
  }

  private truncateUrl(url?: string): string {
    if (!url) return "—";
    if (url.length <= 50) return url;
    return url.slice(0, 24) + "…" + url.slice(-24);
  }

  private async _setDefaultProvider(provider: string): Promise<void> {
    const gw = this.gateway;
    if (!gw || !provider) return;
    this.settingDefault = true;
    try {
      await gw.request("config.set", { default_provider: provider });
      this.defaultProvider = provider;
      this.providers = this.providers.map((p) => ({
        ...p,
        is_default: (p.name ?? "") === provider,
      }));
      ScToast.show({ message: `${provider} set as default provider`, variant: "success" });
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : "Failed to set default",
        variant: "error",
      });
    } finally {
      this.settingDefault = false;
    }
  }

  private async _onProviderChange(e: CustomEvent<{ value: string }>): Promise<void> {
    const val = e.detail.value;
    if (!val) return;
    await this._setDefaultProvider(val);
  }

  private async _onModelChange(e: CustomEvent<{ value: string }>): Promise<void> {
    const gw = this.gateway;
    const val = e.detail.value?.trim();
    if (!gw) return;
    this.settingDefault = true;
    try {
      await gw.request("config.set", { default_model: val });
      this.defaultModel = val;
      ScToast.show({ message: "Default model updated", variant: "success" });
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to set default model",
        variant: "error",
      });
    } finally {
      this.settingDefault = false;
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      <hu-page-hero role="region" aria-label="Models and providers">
        <hu-section-header
          heading="Models & Providers"
          description="AI model providers and their configurations"
        >
          <div class="search-wrap" role="group" aria-label="Search providers">
            <hu-search
              placeholder="Search providers..."
              @hu-search=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
              @hu-clear=${() => (this.filter = "")}
            ></hu-search>
          </div>
          <hu-button
            variant="ghost"
            size="sm"
            ?disabled=${this._refreshing}
            @click=${() => this.load()}
            aria-label="Refresh models"
          >
            ${icons.refresh} ${this._refreshing ? "Refreshing..." : "Refresh"}
          </hu-button>
        </hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-stat-card
          .value=${this.providers.length}
          label="Providers"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.providers.filter((p) => p.has_key).length}
          label="Configured"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.providers.filter((p) => p.native_tools).length}
          label="Native Tools"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.usageByProvider.length}
          label="Usage Providers"
          style="--hu-stagger-delay: 150ms"
        ></hu-stat-card>
      </hu-stats-row>
      ${this.error
        ? html`<hu-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></hu-empty-state>`
        : nothing}
      ${!this.error
        ? html`${this._renderInfoSection()}${this._renderChart()}${this._renderGrid()}`
        : nothing}
    `;
  }

  private _renderSkeleton() {
    return html`
      <hu-page-hero role="region" aria-label="Models and providers">
        <hu-section-header
          heading="Models"
          description="AI model providers and their configurations"
        ></hu-section-header>
      </hu-page-hero>
      <div class="stats-row">
        <hu-skeleton variant="card" height="90px" style="--hu-stagger-delay: 0ms"></hu-skeleton>
        <hu-skeleton variant="card" height="90px" style="--hu-stagger-delay: 50ms"></hu-skeleton>
        <hu-skeleton variant="card" height="90px" style="--hu-stagger-delay: 100ms"></hu-skeleton>
        <hu-skeleton variant="card" height="90px" style="--hu-stagger-delay: 150ms"></hu-skeleton>
      </hu-stats-row>
      <div class="info-section hu-stagger">
        <hu-skeleton variant="card" height="80px"></hu-skeleton>
        <hu-skeleton variant="card" height="80px"></hu-skeleton>
      </div>
      <div class="grid hu-stagger">
        <hu-skeleton variant="card" height="120px"></hu-skeleton>
        <hu-skeleton variant="card" height="120px"></hu-skeleton>
        <hu-skeleton variant="card" height="120px"></hu-skeleton>
      </div>
    `;
  }

  private _renderInfoSection() {
    return html`
      <hu-card>
        <div class="info-section hu-stagger">
          <div class="info-item">
            <span class="section-label">Default provider</span>
            <hu-combobox
              .options=${this.providerOptions}
              .value=${this.defaultProvider}
              label=""
              placeholder="Select provider"
              ?disabled=${this.settingDefault || this.providerOptions.length === 0}
              @hu-combobox-change=${this._onProviderChange}
            ></hu-combobox>
          </div>
          <div class="info-item">
            <span class="section-label">Default model</span>
            <hu-combobox
              .options=${[]}
              .value=${this.defaultModel}
              freeText
              label=""
              placeholder="Model name"
              ?disabled=${this.settingDefault}
              @hu-combobox-change=${this._onModelChange}
            ></hu-combobox>
          </div>
        </div>
      </hu-card>
    `;
  }

  private _renderChart() {
    const data = this.requestDistributionChartData;
    if (!data) return nothing;
    return html`
      <div class="chart-section hu-stagger">
        <div class="chart-header">Request distribution by provider</div>
        <hu-chart type="doughnut" .data=${data} height="200"></hu-chart>
      </div>
    `;
  }

  private _renderGrid() {
    const filtered = this.filteredProviders;
    return html`
      <div class="grid hu-stagger">
        ${filtered.length === 0
          ? html`
              <hu-empty-state
                .icon=${icons.cpu}
                heading=${this.filter ? "No matching providers" : "No providers configured"}
                description=${this.filter
                  ? "Try a different search term."
                  : "Configure an AI provider in your config to get started."}
              ></hu-empty-state>
            `
          : filtered.map((p) => this._renderProviderCard(p))}
      </div>
    `;
  }

  private _renderProviderCard(p: ProviderItem) {
    const isDefault = p.is_default ?? (p.name ?? "") === this.defaultProvider;
    return html`
      <hu-card aria-label=${`Provider: ${p.name ?? "unnamed"}`}>
        <div class="card-header">
          <span class="card-name ${isDefault ? "default" : ""}">${p.name ?? "unnamed"}</span>
          ${isDefault ? html`<hu-badge variant="info">default</hu-badge>` : nothing}
          ${p.native_tools ? html`<hu-badge variant="neutral">native tools</hu-badge>` : nothing}
        </div>
        <div class="key-status ${p.has_key ? "has" : "missing"}">
          <span class="key-icon">${p.has_key ? icons.check : icons["x-circle"]}</span>
          ${p.has_key ? " API key" : " No API key"}
        </div>
        <div class="card-url" title=${p.base_url ?? ""}>${this.truncateUrl(p.base_url)}</div>
        ${!isDefault
          ? html`
              <div class="card-actions">
                <hu-button
                  variant="ghost"
                  size="sm"
                  ?disabled=${this.settingDefault}
                  @click=${() => this._setDefaultProvider(p.name ?? "")}
                  aria-label=${`Set ${p.name ?? "unnamed"} as default`}
                >
                  Set as default
                </hu-button>
              </div>
            `
          : nothing}
      </hu-card>
    `;
  }
}
