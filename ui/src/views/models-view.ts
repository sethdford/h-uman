import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
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
    }
    h2 {
      margin: 0 0 var(--sc-space-md);
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .info-bar {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-lg);
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
      gap: var(--sc-space-md);
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
      color: var(--sc-accent);
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
    if (this.loading) {
      return html`
        <h2>Models & Providers</h2>
        <div class="grid">
          <div class="card skeleton skeleton-card"></div>
          <div class="card skeleton skeleton-card"></div>
          <div class="card skeleton skeleton-card"></div>
        </div>
      `;
    }

    return html`
      <h2>Models & Providers</h2>
      ${this.error ? html`<div class="error-banner">${this.error}</div>` : nothing}
      <div class="info-bar">
        <span class="info-item"
          ><strong>Default provider:</strong> ${this.defaultProvider || "—"}</span
        >
        <span class="info-item"><strong>Default model:</strong> ${this.defaultModel || "—"}</span>
      </div>
      <div class="grid">
        ${this.providers.length === 0
          ? html`
              <div class="empty-state">
                <div class="empty-icon">🤖</div>
                <p class="empty-title">No providers configured</p>
                <p class="empty-desc">Configure an AI provider in your config to get started.</p>
              </div>
            `
          : this.providers.map(
              (p) => html`
                <div class="card">
                  <div class="card-header">
                    <span class="card-name ${p.is_default ? "default" : ""}"
                      >${p.name ?? "unnamed"}</span
                    >
                    ${p.is_default ? html`<span class="badge default">default</span>` : nothing}
                    ${p.native_tools ? html`<span class="badge">native tools</span>` : nothing}
                  </div>
                  <div class="key-status ${p.has_key ? "has" : "missing"}">
                    ${p.has_key ? "✓ API key" : "✗ No API key"}
                  </div>
                  <div class="card-url" title=${p.base_url ?? ""}>
                    ${this.truncateUrl(p.base_url)}
                  </div>
                </div>
              `,
            )}
      </div>
    `;
  }
}
