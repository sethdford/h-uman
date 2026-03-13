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
import "../components/hu-empty-state.js";
import "../components/hu-skeleton.js";
import "../components/hu-badge.js";
import "../components/hu-select.js";
import "../components/hu-switch.js";
import "../components/hu-button.js";

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

interface PairingInfo {
  device_name?: string;
  paired_at?: number;
}

const AUTONOMY_OPTIONS = [
  { value: "0", label: "0 — Read-Only" },
  { value: "1", label: "1 — Supervised" },
  { value: "2", label: "2 — Full Autonomy" },
];

const AUTONOMY_LABELS: Record<number, { label: string; color: string; description: string }> = {
  0: {
    label: "Read-Only",
    color: "var(--hu-success)",
    description: "Agent can only observe and report. No side effects.",
  },
  1: {
    label: "Supervised",
    color: "var(--hu-success)",
    description: "Agent proposes actions. User must approve before execution.",
  },
  2: {
    label: "Full Autonomy",
    color: "var(--hu-error)",
    description: "Agent executes tool calls without confirmation.",
  },
};

@customElement("hu-security-view")
export class ScSecurityView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-security;
      display: block;
      color: var(--hu-text);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(17.5rem, 1fr));
      gap: var(--hu-space-xl);
      margin-bottom: var(--hu-space-2xl);
    }
    .card-inner {
      padding: var(--hu-space-md);
    }
    .card-title {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-sm);
    }
    .domains-label {
      margin-top: var(--hu-space-xs);
    }
    .autonomy-badge-wrap {
      margin-bottom: var(--hu-space-sm);
    }
    .risk-indicator {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-radius: var(--hu-radius);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      margin-bottom: var(--hu-space-sm);
    }
    .risk-indicator.success {
      background: color-mix(in srgb, var(--hu-success) 20%, transparent);
      color: var(--hu-success);
    }
    .risk-indicator.warning {
      background: color-mix(in srgb, var(--hu-warning) 20%, transparent);
      color: var(--hu-warning);
    }
    .risk-indicator.error {
      background: color-mix(in srgb, var(--hu-error) 20%, transparent);
      color: var(--hu-error);
    }
    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      line-height: 1.5;
    }
    .policy-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-xs) 0;
      border-bottom: 1px solid var(--hu-border-subtle);
      font-size: var(--hu-text-sm);
    }
    .policy-row:last-child {
      border-bottom: none;
    }
    .policy-label {
      color: var(--hu-text-muted);
    }
    .policy-value {
      font-weight: var(--hu-weight-medium);
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
    }
    .policy-value.enabled {
      color: var(--hu-success);
    }
    .policy-value.disabled {
      color: var(--hu-text-muted);
    }
    .policy-value.warning {
      color: var(--hu-warning);
    }
    .control-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      margin-top: var(--hu-space-md);
    }
    .switch-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-sm) 0;
    }
    .domain-list {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      margin-top: var(--hu-space-xs);
    }
    .domain-tag {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      background: var(--hu-bg-elevated);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
    }
    .section-title {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin: var(--hu-space-2xl) 0 var(--hu-space-sm);
    }
    .checklist {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }
    .check-item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }
    .check-icon {
      flex-shrink: 0;
      width: 1rem;
      height: 1rem;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .check-icon svg {
      width: 100%;
      height: 100%;
    }
    .check-icon.pass {
      color: var(--hu-success);
    }
    .check-icon.warn {
      color: var(--hu-warning);
    }
    .pairing-info {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      margin-top: var(--hu-space-xs);
    }
    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
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

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

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

  private get pairingInfo(): PairingInfo | null {
    const gw = this.rawConfig?.gateway as { device_name?: string; paired_at?: number } | undefined;
    if (!gw?.device_name && !gw?.paired_at) return null;
    return { device_name: gw.device_name, paired_at: gw.paired_at };
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

  private get netProxyEnabled(): boolean {
    return this.config?.sandbox_config?.net_proxy?.enabled === true;
  }

  private get securityScore(): number {
    let score = 0;
    if (this.sandboxEnabled) score += 25;
    if (this.netProxyEnabled) score += 25;
    if (this.autonomyLevel === 0) score += 50;
    else if (this.autonomyLevel === 1) score += 50;
    else if (this.autonomyLevel === 2) score += 0;
    if (this.pairingEnabled) score += 25;
    if (this.httpsOnly) score += 25;
    return Math.min(100, score);
  }

  private get riskVariant(): "success" | "warning" | "error" {
    if (this.autonomyLevel === 0) return "success";
    if (this.autonomyLevel === 1) return "success";
    return "error";
  }

  private _renderSkeleton() {
    return html`<hu-skeleton variant="card" height="200px"></hu-skeleton>`;
  }

  private _renderAutonomy() {
    const info = this.autonomyInfo;
    const variant = this.riskVariant;
    return html`
      <hu-card>
        <div class="card-inner">
          <div class="card-title">Autonomy Level</div>
          <div class="risk-indicator ${variant}">
            <span>Level ${this.autonomyLevel} &mdash; ${info.label}</span>
          </div>
          <div class="description">${info.description}</div>
          <div class="control-row">
            <hu-select
              label="Change level"
              .options=${AUTONOMY_OPTIONS}
              .value=${String(this.autonomyLevel)}
              @hu-change=${this._onAutonomyChange}
            ></hu-select>
          </div>
        </div>
      </hu-card>
    `;
  }

  private async _onAutonomyChange(e: CustomEvent<{ value: string }>): Promise<void> {
    const level = parseInt(e.detail.value, 10);
    const gw = this.gateway;
    if (!gw) return;
    try {
      await gw.request("config.set", {
        key: "security.autonomy_level",
        value: level,
      });
      if (this.config) {
        this.config = { ...this.config, autonomy_level: level };
      }
      ScToast.show({ message: `Autonomy level set to ${level}`, variant: "success" });
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to update",
        variant: "error",
      });
    }
  }

  private async _onSandboxToggle(e: CustomEvent<{ checked: boolean }>): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    const enabled = e.detail.checked;
    try {
      await gw.request("config.set", {
        key: "security.sandbox_config.enabled",
        value: enabled,
      });
      if (this.config?.sandbox_config) {
        this.config = {
          ...this.config,
          sandbox_config: { ...this.config.sandbox_config, enabled },
        };
      } else if (this.config) {
        this.config = {
          ...this.config,
          sandbox_config: { enabled, net_proxy: this.config.sandbox_config?.net_proxy },
        };
      }
      ScToast.show({ message: `Sandbox ${enabled ? "enabled" : "disabled"}`, variant: "success" });
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to update",
        variant: "error",
      });
    }
  }

  private async _onNetProxyToggle(e: CustomEvent<{ checked: boolean }>): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    const enabled = e.detail.checked;
    const netProxy = this.config?.sandbox_config?.net_proxy ?? {};
    try {
      await gw.request("config.set", {
        key: "security.sandbox_config.net_proxy.enabled",
        value: enabled,
      });
      if (this.config?.sandbox_config) {
        this.config = {
          ...this.config,
          sandbox_config: {
            ...this.config.sandbox_config,
            net_proxy: { ...netProxy, enabled },
          },
        };
      }
      ScToast.show({
        message: `Network proxy ${enabled ? "enabled" : "disabled"}`,
        variant: "success",
      });
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to update",
        variant: "error",
      });
    }
  }

  private _renderSandbox() {
    if (!this.config) return nothing;
    return html`
      <hu-card>
        <div class="card-inner">
          <div class="card-title">Sandbox</div>
          <div class="policy-row">
            <span class="policy-label">Backend</span>
            <span class="policy-value">${this.sandboxBackend}</span>
          </div>
          <div class="switch-row">
            <span class="policy-label">Enabled</span>
            <hu-switch
              checked=${this.sandboxEnabled}
              label="Enable sandbox"
              @hu-change=${this._onSandboxToggle}
            ></hu-switch>
          </div>
        </div>
      </hu-card>
    `;
  }

  private _renderNetwork() {
    const proxy = this.config?.sandbox_config?.net_proxy;
    return html`
      <hu-card>
        <div class="card-inner">
          <div class="card-title">Network Proxy</div>
          <div class="switch-row">
            <span class="policy-label">Enabled</span>
            <hu-switch
              checked=${this.netProxyEnabled}
              label="Enable network proxy"
              @hu-change=${this._onNetProxyToggle}
            ></hu-switch>
          </div>
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
      </hu-card>
    `;
  }

  private _renderPairing() {
    if (!this.pairingEnabled) return nothing;
    const info = this.pairingInfo;
    const pairedDate = info?.paired_at
      ? new Date(info.paired_at * 1000).toLocaleDateString(undefined, {
          dateStyle: "medium",
        })
      : null;
    return html`
      <hu-card>
        <div class="card-inner">
          <div class="card-title">Pairing</div>
          <div class="policy-row">
            <span class="policy-label">Status</span>
            <span class="policy-value enabled">Paired</span>
          </div>
          ${info?.device_name || pairedDate
            ? html`
                <div class="pairing-info">
                  ${info?.device_name ? html`<div>Device: ${info.device_name}</div>` : nothing}
                  ${pairedDate ? html`<div>Paired: ${pairedDate}</div>` : nothing}
                </div>
              `
            : nothing}
          <div class="control-row">
            <hu-button
              variant="secondary"
              size="sm"
              @click=${this._onUnpair}
              aria-label="Unpair device"
              >Unpair</hu-button
            >
          </div>
        </div>
      </hu-card>
    `;
  }

  private async _onUnpair(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    try {
      await gw.request("config.set", {
        key: "gateway.require_pairing",
        value: false,
      });
      if (this.rawConfig?.gateway && typeof this.rawConfig.gateway === "object") {
        this.rawConfig = {
          ...this.rawConfig,
          gateway: {
            ...(this.rawConfig.gateway as Record<string, unknown>),
            require_pairing: false,
          },
        };
      }
      ScToast.show({ message: "Pairing disabled", variant: "success" });
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to unpair",
        variant: "error",
      });
    }
  }

  private _renderDefaults() {
    return html`
      <div class="section-title">Security Defaults</div>
      <hu-card>
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
      </hu-card>
    `;
  }

  override render() {
    const hero = html`
      <hu-page-hero role="region" aria-label="Security">
        <hu-section-header
          heading="Security"
          description="Access control, pairing, and security policies"
        ></hu-section-header>
      </hu-page-hero>
    `;

    if (this.loading) {
      return html`${hero} ${this._renderSkeleton()}`;
    }

    if (this.error) {
      return html`${hero}
        <hu-empty-state
          .icon=${icons.warning}
          heading="Error"
          description=${this.error}
        ></hu-empty-state>`;
    }

    if (!this.config) {
      return html`${hero}
        <hu-empty-state
          heading="Security Policy"
          description="Connect to a h-uman gateway to view security settings."
        ></hu-empty-state>`;
    }

    return html`
      ${hero}
      <hu-stats-row>
        <hu-stat-card
          .value=${this.securityScore}
          label="Security Score"
          accent=${this.securityScore >= 75
            ? "primary"
            : this.securityScore >= 50
              ? "secondary"
              : "error"}
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.pairingEnabled ? 1 : 0}
          label="Pairing"
          accent=${this.pairingEnabled ? "primary" : "error"}
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.httpsOnly ? 1 : 0}
          label="HTTPS Only"
          accent=${this.httpsOnly ? "primary" : "error"}
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.sandboxEnabled ? 1 : 0}
          label="Sandbox"
          accent=${this.sandboxEnabled ? "primary" : "error"}
          style="--hu-stagger-delay: 150ms"
        ></hu-stat-card>
      </hu-stats-row>
      <div class="grid">
        ${this._renderAutonomy()} ${this._renderSandbox()} ${this._renderNetwork()}
        ${this._renderPairing()}
      </div>
      ${this._renderDefaults()}
    `;
  }
}
