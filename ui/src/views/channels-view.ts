import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

interface ChannelStatus {
  key?: string;
  label?: string;
  name?: string;
  status?: string;
  healthy?: boolean;
  configured?: boolean;
  build_enabled?: boolean;
  error?: string;
}

@customElement("sc-channels-view")
export class ScChannelsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
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
    }
    .card-name {
      font-weight: 600;
      font-size: 1rem;
      color: var(--sc-text);
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
    }
    .status-dot.healthy {
      background: var(--sc-success);
    }
    .status-dot.error {
      background: var(--sc-error);
    }
    .status-dot.unconfigured {
      background: var(--sc-text-muted);
    }
    .card-info {
      font-size: 0.8125rem;
      color: var(--sc-text-muted);
    }
    .card-info .error {
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

  @state() private channels: ChannelStatus[] = [];
  @state() private loading = true;
  @state() private error = "";

  protected override async load(): Promise<void> {
    await this.loadChannels();
  }

  private async loadChannels(): Promise<void> {
    if (!this.gateway) {
      this.loading = false;
      return;
    }
    this.loading = true;
    try {
      const payload = await this.gateway.request<{
        channels?: ChannelStatus[];
      }>("channels.status", {});
      this.channels = payload?.channels ?? [];
    } catch (e) {
      this.channels = [];
      this.error = e instanceof Error ? e.message : "Failed to load channels";
    } finally {
      this.loading = false;
    }
  }

  private dotClass(ch: ChannelStatus): string {
    if (ch.healthy === true) return "healthy";
    if (ch.configured === false || ch.status === "unconfigured") return "unconfigured";
    return "error";
  }

  override render() {
    if (this.loading) {
      return html`
        <div class="grid">
          <div class="card skeleton skeleton-card"></div>
          <div class="card skeleton skeleton-card"></div>
          <div class="card skeleton skeleton-card"></div>
        </div>
      `;
    }

    return html`
      ${this.error ? html`<div class="error-banner">${this.error}</div>` : nothing}
      <div class="grid">
        ${this.channels.length === 0
          ? html`
              <div class="empty-state">
                <div class="empty-icon">📡</div>
                <p class="empty-title">No channels configured</p>
                <p class="empty-desc">Configure messaging channels to receive and send messages.</p>
              </div>
            `
          : this.channels.map(
              (ch) => html`
                <div class="card">
                  <div class="card-header">
                    <span class="status-dot ${this.dotClass(ch)}"></span>
                    <span class="card-name">${ch.label || ch.key || ch.name || "unnamed"}</span>
                  </div>
                  <div class="card-info">
                    ${ch.error ? html`<span class="error">${ch.error}</span>` : (ch.status ?? "—")}
                  </div>
                </div>
              `,
            )}
      </div>
    `;
  }
}
