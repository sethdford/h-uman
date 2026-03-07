import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";

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

@customElement("sc-models-view")
export class ScModelsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      max-width: 1200px;
    }
    h2 {
      margin: 0 0 var(--sc-space-xl);
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .info-bar {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
      font-size: var(--sc-text-base);
    }
    .info-item {
      color: var(--sc-text-muted);
    }
    .info-item strong {
      color: var(--sc-text);
      margin-right: var(--sc-space-xs);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
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
    .key-status svg {
      width: 14px;
      height: 14px;
    }
    .key-icon {
      width: 14px;
      height: 14px;
      display: inline-block;
      vertical-align: middle;
    }
    .info-card {
      margin-bottom: var(--sc-space-2xl);
    }
    @media (max-width: 768px) {
      .grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 480px) {
      .grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private defaultModel = "";
  @state() private defaultProvider = "";
  @state() private providers: ProviderItem[] = [];
  @state() private loading = true;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.loading = false;
      return;
    }
    this.loading = true;
    try {
      const [modelsRes, configRes] = await Promise.all([
        gw.request<ModelsListRes>("models.list", {}).catch((): Partial<ModelsListRes> => ({})),
        gw.request<ConfigGetRes>("config.get", {}).catch((): Partial<ConfigGetRes> => ({})),
      ]);
      this.defaultModel = modelsRes?.default_model ?? configRes?.default_model ?? "";
      this.defaultProvider = configRes?.default_provider ?? "";
      this.providers = modelsRes?.providers ?? [];
    } catch (e) {
      this.providers = [];
      this.defaultModel = "";
      this.defaultProvider = "";
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

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      <h2>Models & Providers</h2>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this._renderInfoBar()} ${this._renderGrid()}
    `;
  }

  private _renderSkeleton() {
    return html`
      <h2>Models & Providers</h2>
      <div class="grid sc-stagger">
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
      </div>
    `;
  }

  private _renderInfoBar() {
    return html`
      <sc-card class="info-card">
        <div class="info-bar">
          <span class="info-item"
            ><strong>Default provider:</strong> ${this.defaultProvider || "\u2014"}</span
          >
          <span class="info-item"
            ><strong>Default model:</strong> ${this.defaultModel || "\u2014"}</span
          >
        </div>
      </sc-card>
    `;
  }

  private _renderGrid() {
    return html`
      <div class="grid sc-stagger">
        ${this.providers.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons.cpu}
                heading="No providers configured"
                description="Configure an AI provider in your config to get started."
              ></sc-empty-state>
            `
          : this.providers.map((p) => this._renderProviderCard(p))}
      </div>
    `;
  }

  private _renderProviderCard(p: ProviderItem) {
    return html`
      <sc-card>
        <div class="card-header">
          <span class="card-name ${p.is_default ? "default" : ""}">${p.name ?? "unnamed"}</span>
          ${p.is_default ? html`<sc-badge variant="info">default</sc-badge>` : nothing}
          ${p.native_tools ? html`<sc-badge variant="neutral">native tools</sc-badge>` : nothing}
        </div>
        <div class="key-status ${p.has_key ? "has" : "missing"}">
          <span class="key-icon">${p.has_key ? icons.check : icons["x-circle"]}</span>
          ${p.has_key ? " API key" : " No API key"}
        </div>
        <div class="card-url" title=${p.base_url ?? ""}>${this.truncateUrl(p.base_url)}</div>
      </sc-card>
    `;
  }
}
