import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

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
      margin: 0 0 1rem;
      font-size: 1.25rem;
      font-weight: 600;
      color: var(--sc-text);
    }
    .info-bar {
      display: flex;
      flex-wrap: wrap;
      gap: 1rem;
      padding: 0.75rem 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      margin-bottom: 1.5rem;
      font-size: 0.875rem;
    }
    .info-item {
      color: var(--sc-text-muted);
    }
    .info-item strong {
      color: var(--sc-text);
      margin-right: 0.25rem;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: 1rem;
    }
    .card {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      padding: 1rem;
    }
    .card-header {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      margin-bottom: 0.5rem;
      flex-wrap: wrap;
    }
    .card-name {
      font-weight: 600;
      font-size: 1rem;
      color: var(--sc-text);
    }
    .card-name.default {
      color: var(--sc-accent);
    }
    .badge {
      font-size: 0.625rem;
      padding: 0.2rem 0.4rem;
      border-radius: 4px;
      background: var(--sc-bg-elevated);
      color: var(--sc-text-muted);
    }
    .badge.default {
      background: var(--sc-accent);
      color: var(--sc-bg);
    }
    .card-url {
      font-size: 0.75rem;
      font-family: var(--sc-font-mono);
      color: var(--sc-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      margin-top: 0.25rem;
    }
    .key-status {
      font-size: 0.8125rem;
      margin-top: 0.5rem;
      display: flex;
      align-items: center;
      gap: 0.35rem;
    }
    .key-status.has {
      color: var(--sc-success);
    }
    .key-status.missing {
      color: var(--sc-error);
    }
    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-surface) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
      animation: sc-shimmer 1.5s ease-in-out infinite;
      border-radius: var(--sc-radius);
    }
    .skeleton-line {
      height: 1rem;
      margin-bottom: 0.75rem;
      border-radius: 4px;
    }
    .skeleton-card {
      height: 5rem;
      margin-bottom: 0.75rem;
    }
    .empty-state {
      text-align: center;
      padding: 3rem 1rem;
      color: var(--sc-text-muted);
    }
    .empty-icon {
      font-size: 2.5rem;
      margin-bottom: 1rem;
    }
    .empty-title {
      font-size: var(--sc-text-lg);
      font-weight: 600;
      color: var(--sc-text);
      margin: 0 0 0.5rem;
    }
    .empty-desc {
      font-size: var(--sc-text-sm);
      margin: 0;
      max-width: 24rem;
      margin-inline: auto;
    }
    .grid .empty-state {
      grid-column: 1 / -1;
    }
    .error-banner {
      background: var(--sc-error);
      color: #fff;
      padding: 0.75rem 1rem;
      border-radius: 8px;
      margin-bottom: 1rem;
      font-size: var(--sc-text-sm);
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
