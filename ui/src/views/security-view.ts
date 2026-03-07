import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-stat-card.js";
import "../components/sc-card.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";
import "../components/sc-badge.js";

interface SecurityConfig {
  autonomy_level?: number;
  sandbox?: string;
  sandbox_config?: {
    enabled?: boolean;
    backend?: string;
    net_proxy?: {
      enabled?: boolean;
      deny_all?: boolean;
      proxy_addr?: string;
      allowed_domains?: string[];
    };
  };
}

const AUTONOMY_LABELS: Record<number, { label: string; color: string; description: string }> = {
  0: {
    label: "Read-Only",
    color: "var(--sc-success)",
    description: "Agent can only observe and report. No side effects.",
  },
  1: {
    label: "Supervised",
    color: "var(--sc-warning)",
    description: "Agent proposes actions. User must approve before execution.",
  },
  2: {
    label: "Full Autonomy",
    color: "var(--sc-error)",
    description: "Agent executes tool calls without confirmation.",
  },
};

@customElement("sc-security-view")
export class ScSecurityView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
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
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
    }
    .card-inner {
      padding: var(--sc-space-md);
    }
    .card-title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-sm);
    }
    .domains-label {
      margin-top: var(--sc-space-xs);
    }
    .autonomy-badge-wrap {
      margin-bottom: var(--sc-space-sm);
    }
    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      line-height: 1.5;
    }
    .policy-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-xs) 0;
      border-bottom: 1px solid var(--sc-border-subtle);
      font-size: var(--sc-text-sm);
    }
    .policy-row:last-child {
      border-bottom: none;
    }
    .policy-label {
      color: var(--sc-text-muted);
    }
    .policy-value {
      font-weight: var(--sc-weight-medium);
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-xs);
    }
    .policy-value.enabled {
      color: var(--sc-success);
    }
    .policy-value.disabled {
      color: var(--sc-text-muted);
    }
    .policy-value.warning {
      color: var(--sc-warning);
    }
    .domain-list {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-xs);
      margin-top: var(--sc-space-xs);
    }
    .domain-tag {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-xs);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      background: var(--sc-bg-elevated);
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
    }
    .section-title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: var(--sc-space-2xl) 0 var(--sc-space-sm);
    }
    .checklist {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }
    .check-item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .check-icon {
      flex-shrink: 0;
      width: 16px;
      height: 16px;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .check-icon svg {
      width: 100%;
      height: 100%;
    }
    .check-icon.pass {
      color: var(--sc-success);
    }
    .check-icon.warn {
      color: var(--sc-warning);
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
    @media (prefers-reduced-motion: reduce) {
      :host * {
        animation: none !important;
        transition: none !important;
      }
    }
  `;

  @state() private config: SecurityConfig | null = null;
  @state() private rawConfig: Record<string, unknown> | null = null;
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const res = await gw.request<Record<string, unknown>>("config.get", {});
      this.rawConfig = res ?? null;
      if (res && typeof res === "object" && "security" in res) {
        this.config = res.security as SecurityConfig;
      } else {
        this.config = {};
      }
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load security config";
    } finally {
      this.loading = false;
    }
  }

  private get pairingEnabled(): boolean {
    const gw = this.rawConfig?.gateway as { require_pairing?: boolean } | undefined;
    return gw?.require_pairing === true;
  }

  private get httpsOnly(): boolean {
    return true;
  }

  private get sandboxEnabled(): boolean {
    return this.config?.sandbox_config?.enabled === true;
  }

  private get autonomyLevel(): number {
    return this.config?.autonomy_level ?? 1;
  }

  private get autonomyInfo() {
    return AUTONOMY_LABELS[this.autonomyLevel] ?? AUTONOMY_LABELS[1];
  }

  private get sandboxBackend(): string {
    return this.config?.sandbox_config?.backend ?? this.config?.sandbox ?? "auto";
  }

  private _renderSkeleton() {
    return html`<sc-skeleton variant="card" height="200px"></sc-skeleton>`;
  }

  private _renderAutonomy() {
    const info = this.autonomyInfo;
    const variant =
      this.autonomyLevel === 0 ? "success" : this.autonomyLevel === 1 ? "warning" : "error";
    return html`
      <sc-card>
        <div class="card-inner">
          <div class="card-title">Autonomy Level</div>
          <div class="autonomy-badge-wrap">
            <sc-badge variant=${variant}
              >Level ${this.autonomyLevel} &mdash; ${info.label}</sc-badge
            >
          </div>
          <div class="description">${info.description}</div>
        </div>
      </sc-card>
    `;
  }

  private _renderSandbox() {
    if (!this.config) return nothing;
    return html`
      <sc-card>
        <div class="card-inner">
          <div class="card-title">Sandbox</div>
          <div class="policy-row">
            <span class="policy-label">Backend</span>
            <span class="policy-value">${this.sandboxBackend}</span>
          </div>
          <div class="policy-row">
            <span class="policy-label">Enabled</span>
            <span
              class="policy-value ${this.config.sandbox_config?.enabled ? "enabled" : "disabled"}"
            >
              ${this.config.sandbox_config?.enabled ? "yes" : "auto-detect"}
            </span>
          </div>
        </div>
      </sc-card>
    `;
  }

  private _renderNetwork() {
    const proxy = this.config?.sandbox_config?.net_proxy;
    return html`
      <sc-card>
        <div class="card-inner">
          <div class="card-title">Network Proxy</div>
          ${proxy?.enabled
            ? html`
                <div class="policy-row">
                  <span class="policy-label">Status</span>
                  <span class="policy-value enabled">active</span>
                </div>
                <div class="policy-row">
                  <span class="policy-label">Deny all</span>
                  <span class="policy-value ${proxy.deny_all ? "warning" : "disabled"}">
                    ${proxy.deny_all ? "yes" : "no"}
                  </span>
                </div>
                ${proxy.proxy_addr
                  ? html`
                      <div class="policy-row">
                        <span class="policy-label">Proxy</span>
                        <span class="policy-value">${proxy.proxy_addr}</span>
                      </div>
                    `
                  : nothing}
                ${proxy.allowed_domains && proxy.allowed_domains.length > 0
                  ? html`
                      <div class="policy-label domains-label">Allowed domains</div>
                      <div class="domain-list">
                        ${proxy.allowed_domains.map(
                          (d) => html`<span class="domain-tag">${d}</span>`,
                        )}
                      </div>
                    `
                  : nothing}
              `
            : html`
                <div class="policy-row">
                  <span class="policy-label">Status</span>
                  <span class="policy-value disabled">not configured</span>
                </div>
              `}
        </div>
      </sc-card>
    `;
  }

  private _renderDefaults() {
    return html`
      <div class="section-title">Security Defaults</div>
      <sc-card>
        <div class="card-inner">
          <div class="checklist">
            <div class="check-item">
              <span class="check-icon pass">${icons.check}</span>
              HTTPS-only enforced for outbound URLs
            </div>
            <div class="check-item">
              <span class="check-icon pass">${icons.check}</span>
              Secrets encrypted at rest (AEAD)
            </div>
            <div class="check-item">
              <span class="check-icon pass">${icons.check}</span>
              Path traversal protection on all file tools
            </div>
            <div class="check-item">
              <span class="check-icon pass">${icons.check}</span>
              No credentials in logs or error messages
            </div>
            <div class="check-item">
              <span class="check-icon ${this.autonomyLevel < 2 ? "pass" : "warn"}">
                ${this.autonomyLevel < 2 ? icons.check : icons.warning}
              </span>
              ${this.autonomyLevel < 2
                ? "User approval required before tool execution"
                : "Tool execution without user approval (autonomy = 2)"}
            </div>
          </div>
        </div>
      </sc-card>
    `;
  }

  override render() {
    const hero = html`
      <sc-page-hero>
        <sc-section-header
          heading="Security"
          description="Access control, pairing, and security policies"
        ></sc-section-header>
      </sc-page-hero>
    `;

    if (this.loading) {
      return html`${hero} ${this._renderSkeleton()}`;
    }

    if (this.error) {
      return html`${hero}
        <sc-empty-state heading="Error" description=${this.error}></sc-empty-state>`;
    }

    if (!this.config) {
      return html`${hero}
        <sc-empty-state
          heading="Security Policy"
          description="Connect to a SeaClaw gateway to view security settings."
        ></sc-empty-state>`;
    }

    return html`
      ${hero}
      <div class="stats-row">
        <sc-stat-card
          .value=${this.pairingEnabled ? 1 : 0}
          label="Pairing"
          accent=${this.pairingEnabled ? "primary" : "error"}
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.httpsOnly ? 1 : 0}
          label="HTTPS Only"
          accent=${this.httpsOnly ? "primary" : "error"}
          style="--sc-stagger-delay: 80ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.sandboxEnabled ? 1 : 0}
          label="Sandbox"
          accent=${this.sandboxEnabled ? "primary" : "error"}
          style="--sc-stagger-delay: 160ms"
        ></sc-stat-card>
      </div>
      <div class="grid">
        ${this._renderAutonomy()} ${this._renderSandbox()} ${this._renderNetwork()}
      </div>
      ${this._renderDefaults()}
    `;
  }
}
