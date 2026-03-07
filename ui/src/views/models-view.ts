import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-stat-card.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-search.js";
import "../components/sc-chart.js";
import "../components/sc-combobox.js";
import type { ChartData } from "../components/sc-chart.js";

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

@customElement("sc-models-view")
export class ScModelsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-models;
      display: block;
      max-width: 75rem;
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
    .info-section {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
    }
    .info-item {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }
    .chart-section {
      margin-bottom: var(--sc-space-2xl);
    }
    .chart-header {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-md);
    }
    .section-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(17.5rem, 1fr));
      gap: var(--sc-space-xl);
    }
    .card-header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-sm);
      flex-wrap: wrap;
    }
    .card-name {
      font-weight: var(--sc-weight-semibold);
      font-size: var(--sc-text-lg);
      color: var(--sc-text);
    }
    .card-name.default {
      color: var(--sc-accent-text, var(--sc-accent));
    }
    .card-url {
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font-mono);
      color: var(--sc-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      margin-top: var(--sc-space-xs);
    }
    .key-status {
      font-size: var(--sc-text-sm);
      margin-top: var(--sc-space-sm);
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
    }
    .key-status.has {
      color: var(--sc-success);
    }
    .key-status.missing {
      color: var(--sc-error);
    }
    .key-icon {
      width: 0.875rem;
      height: 0.875rem;
      display: inline-block;
      vertical-align: middle;
    }
    .card-actions {
      margin-top: var(--sc-space-md);
      display: flex;
      justify-content: flex-end;
    }
    @media (max-width: 48rem) /* --sc-breakpoint-lg */ {
      .info-section {
        grid-template-columns: 1fr;
      }
      .grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .grid {
        grid-template-columns: 1fr;
      }
    }
    .search-wrap {
      margin-bottom: var(--sc-space-xl);
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
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
    this.loading = true;
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
      this.error = e instanceof Error ? e.message : "Failed to load models";
    } finally {
      this.loading = false;
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
      <sc-page-hero role="region" aria-label="Models and providers">
        <sc-section-header
          heading="Models & Providers"
          description="AI model providers and their configurations"
        >
          <div class="search-wrap" role="group" aria-label="Search providers">
            <sc-search
              placeholder="Search providers..."
              @sc-search=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
              @sc-clear=${() => (this.filter = "")}
            ></sc-search>
          </div>
        </sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-stat-card
          .value=${this.providers.length}
          label="Providers"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.providers.filter((p) => p.has_key).length}
          label="Configured"
          style="--sc-stagger-delay: 50ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.providers.filter((p) => p.native_tools).length}
          label="Native Tools"
          style="--sc-stagger-delay: 100ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.usageByProvider.length}
          label="Usage Providers"
          style="--sc-stagger-delay: 150ms"
        ></sc-stat-card>
      </div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${!this.error
        ? html`${this._renderInfoSection()}${this._renderChart()}${this._renderGrid()}`
        : nothing}
    `;
  }

  private _renderSkeleton() {
    return html`
      <sc-page-hero role="region" aria-label="Models and providers">
        <sc-section-header
          heading="Models"
          description="AI model providers and their configurations"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-skeleton variant="card" height="90px" style="--sc-stagger-delay: 0ms"></sc-skeleton>
        <sc-skeleton variant="card" height="90px" style="--sc-stagger-delay: 50ms"></sc-skeleton>
        <sc-skeleton variant="card" height="90px" style="--sc-stagger-delay: 100ms"></sc-skeleton>
        <sc-skeleton variant="card" height="90px" style="--sc-stagger-delay: 150ms"></sc-skeleton>
      </div>
      <div class="info-section sc-stagger">
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
      </div>
      <div class="grid sc-stagger">
        <sc-skeleton variant="card" height="120px"></sc-skeleton>
        <sc-skeleton variant="card" height="120px"></sc-skeleton>
        <sc-skeleton variant="card" height="120px"></sc-skeleton>
      </div>
    `;
  }

  private _renderInfoSection() {
    return html`
      <sc-card>
        <div class="info-section sc-stagger">
          <div class="info-item">
            <span class="section-label">Default provider</span>
            <sc-combobox
              .options=${this.providerOptions}
              .value=${this.defaultProvider}
              label=""
              placeholder="Select provider"
              ?disabled=${this.settingDefault || this.providerOptions.length === 0}
              @sc-combobox-change=${this._onProviderChange}
            ></sc-combobox>
          </div>
          <div class="info-item">
            <span class="section-label">Default model</span>
            <sc-combobox
              .options=${[]}
              .value=${this.defaultModel}
              freeText
              label=""
              placeholder="Model name"
              ?disabled=${this.settingDefault}
              @sc-combobox-change=${this._onModelChange}
            ></sc-combobox>
          </div>
        </div>
      </sc-card>
    `;
  }

  private _renderChart() {
    const data = this.requestDistributionChartData;
    if (!data) return nothing;
    return html`
      <div class="chart-section sc-stagger">
        <div class="chart-header">Request distribution by provider</div>
        <sc-chart type="doughnut" .data=${data} height="200"></sc-chart>
      </div>
    `;
  }

  private _renderGrid() {
    const filtered = this.filteredProviders;
    return html`
      <div class="grid sc-stagger">
        ${filtered.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons.cpu}
                heading=${this.filter ? "No matching providers" : "No providers configured"}
                description=${this.filter
                  ? "Try a different search term."
                  : "Configure an AI provider in your config to get started."}
              ></sc-empty-state>
            `
          : filtered.map((p) => this._renderProviderCard(p))}
      </div>
    `;
  }

  private _renderProviderCard(p: ProviderItem) {
    const isDefault = p.is_default ?? (p.name ?? "") === this.defaultProvider;
    return html`
      <sc-card aria-label=${`Provider: ${p.name ?? "unnamed"}`}>
        <div class="card-header">
          <span class="card-name ${isDefault ? "default" : ""}">${p.name ?? "unnamed"}</span>
          ${isDefault ? html`<sc-badge variant="info">default</sc-badge>` : nothing}
          ${p.native_tools ? html`<sc-badge variant="neutral">native tools</sc-badge>` : nothing}
        </div>
        <div class="key-status ${p.has_key ? "has" : "missing"}">
          <span class="key-icon">${p.has_key ? icons.check : icons["x-circle"]}</span>
          ${p.has_key ? " API key" : " No API key"}
        </div>
        <div class="card-url" title=${p.base_url ?? ""}>${this.truncateUrl(p.base_url)}</div>
        ${!isDefault
          ? html`
              <div class="card-actions">
                <sc-button
                  variant="ghost"
                  size="sm"
                  ?disabled=${this.settingDefault}
                  @click=${() => this._setDefaultProvider(p.name ?? "")}
                  aria-label=${`Set ${p.name ?? "unnamed"} as default`}
                >
                  Set as default
                </sc-button>
              </div>
            `
          : nothing}
      </sc-card>
    `;
  }
}
