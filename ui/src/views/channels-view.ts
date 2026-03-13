import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/hu-data-table-v2.js";
import type { SegmentOption } from "../components/hu-segmented-control.js";
import "../components/hu-badge.js";
import "../components/hu-data-table-v2.js";
import "../components/hu-empty-state.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-sheet.js";
import "../components/hu-skeleton.js";
import "../components/hu-segmented-control.js";
import "../components/hu-button.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import { friendlyError } from "../utils/friendly-error.js";

interface ChannelStatus {
  key?: string;
  label?: string;
  name?: string;
  status?: string;
  healthy?: boolean;
  configured?: boolean;
  build_enabled?: boolean;
  error?: string;
  last_active?: string;
}

type FilterValue = "all" | "configured" | "unconfigured";

@customElement("hu-channels-view")
export class ScChannelsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-channels;
      display: block;
      max-width: 75rem;
    }
    .filters {
      margin-bottom: var(--hu-space-lg);
    }
    .table-section {
      margin-top: var(--hu-space-xl);
    }
    .sheet-detail-row {
      display: flex;
      justify-content: space-between;
      padding: var(--hu-space-sm) 0;
      border-bottom: 1px solid var(--hu-border-subtle);
    }
    .sheet-detail-row:last-child {
      border-bottom: none;
    }
    .sheet-detail-label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }
    .sheet-detail-value {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }
    .sheet-title {
      font-weight: var(--hu-weight-semibold);
      font-size: var(--hu-text-lg);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-lg);
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
    }
  `;

  @state() private channels: ChannelStatus[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private filter: FilterValue = "all";
  @state() private _sheetChannel: ChannelStatus | null = null;
  @state() private _refreshing = false;

  private readonly filterOptions: SegmentOption[] = [
    { value: "all", label: "All" },
    { value: "configured", label: "Configured" },
    { value: "unconfigured", label: "Unconfigured" },
  ];

  protected override async load(): Promise<void> {
    await this.loadChannels();
  }

  private async loadChannels(): Promise<void> {
    if (!this.gateway) {
      this.loading = false;
      return;
    }
    this._refreshing = this.channels.length > 0;
    this.loading = this.channels.length === 0;
    try {
      const payload = await this.gateway.request<{
        channels?: ChannelStatus[];
      }>("channels.status", {});
      this.channels = payload?.channels ?? [];
    } catch (e) {
      this.channels = [];
      this.error = friendlyError(e);
    } finally {
      this.loading = false;
      this._refreshing = false;
    }
  }

  private statusVariant(ch: ChannelStatus): "success" | "error" | "neutral" {
    if (ch.healthy === true || ch.configured === true) return "success";
    if (ch.build_enabled === false || ch.configured === false || ch.status === "unconfigured")
      return "neutral";
    return "error";
  }

  private statusLabel(ch: ChannelStatus): string {
    if (ch.healthy === true || ch.status === "active" || ch.status === "ok") return "Active";
    if (ch.configured === false || ch.status === "unconfigured") return "Unconfigured";
    return "Error";
  }

  private get messagesToday(): number {
    return 0;
  }

  private get filteredChannels(): ChannelStatus[] {
    if (this.filter === "all") return this.channels;
    if (this.filter === "configured") return this.channels.filter((ch) => ch.configured === true);
    return this.channels.filter((ch) => ch.configured !== true);
  }

  private get tableRows(): Record<string, unknown>[] {
    return this.filteredChannels.map((ch) => ({
      name: ch.label || ch.key || ch.name || "unnamed",
      status: this.statusLabel(ch),
      statusVariant: this.statusVariant(ch),
      health: ch.healthy === true ? "Healthy" : ch.healthy === false ? "Unhealthy" : "-",
      lastActive: ch.last_active ?? "-",
      _channel: ch,
    }));
  }

  private readonly columns: DataTableColumnV2[] = [
    { key: "name", label: "Name", sortable: true },
    {
      key: "status",
      label: "Status",
      render: (_v, row) => {
        const variant = row.statusVariant as "success" | "error" | "neutral";
        const label = String(row.status ?? "");
        return html`<hu-badge variant=${variant}>${label}</hu-badge>`;
      },
    },
    { key: "health", label: "Health" },
    { key: "lastActive", label: "Last Active" },
  ];

  private _onRowClick(e: CustomEvent<{ row: Record<string, unknown>; index: number }>): void {
    const ch = e.detail.row._channel as ChannelStatus;
    this._sheetChannel = ch ?? null;
  }

  private _onSheetClose(): void {
    this._sheetChannel = null;
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  private _renderSkeleton(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Channels overview">
        <hu-section-header
          heading="Channels"
          description="Messaging integrations and their connection status"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <div class="table-section">
        <hu-skeleton variant="card" height="200px"></hu-skeleton>
      </div>
    `;
  }

  private _renderContent(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Channels overview">
        <hu-section-header
          heading="Channels"
          description="Messaging integrations and their connection status"
        >
          <hu-button
            variant="ghost"
            size="sm"
            ?disabled=${this._refreshing}
            @click=${() => this.loadChannels()}
            aria-label="Refresh channels"
          >
            ${icons.refresh} ${this._refreshing ? "Refreshing..." : "Refresh"}
          </hu-button>
        </hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-stat-card
          .value=${this.channels.length}
          label="Total Channels"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.channels.filter((ch) => ch.configured === true).length}
          label="Configured"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.channels.filter((ch) => ch.healthy === true).length}
          label="Healthy"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.messagesToday}
          label="Messages Today"
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
      <div class="filters" role="group" aria-label="Filter channels by status">
        <hu-segmented-control
          .options=${this.filterOptions}
          .value=${this.filter}
          @hu-change=${(e: CustomEvent<{ value: string }>) => {
            this.filter = e.detail.value as FilterValue;
          }}
        ></hu-segmented-control>
      </div>
      <div class="table-section" role="region" aria-label="Channels table">
        ${this.filteredChannels.length === 0
          ? html`
              <hu-empty-state
                .icon=${icons.radio}
                heading=${this.filter !== "all" ? "No matching channels" : "No channels configured"}
                description=${this.filter !== "all"
                  ? "Try a different filter."
                  : "Configure messaging channels to receive and send messages."}
              ></hu-empty-state>
            `
          : html`
              <hu-data-table-v2
                .columns=${this.columns}
                .rows=${this.tableRows}
                searchable
                @hu-row-click=${this._onRowClick}
              ></hu-data-table-v2>
            `}
      </div>
      <hu-sheet ?open=${this._sheetChannel != null} @close=${this._onSheetClose}>
        ${this._sheetChannel
          ? html`
              <div class="sheet-title">
                ${this._sheetChannel.label ||
                this._sheetChannel.key ||
                this._sheetChannel.name ||
                "Channel"}
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Status</span>
                <span class="sheet-detail-value">${this.statusLabel(this._sheetChannel)}</span>
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Health</span>
                <span class="sheet-detail-value"
                  >${this._sheetChannel.healthy === true
                    ? "Healthy"
                    : this._sheetChannel.healthy === false
                      ? "Unhealthy"
                      : "-"}</span
                >
              </div>
              <div class="sheet-detail-row">
                <span class="sheet-detail-label">Build enabled</span>
                <span class="sheet-detail-value"
                  >${this._sheetChannel.build_enabled === true ? "Yes" : "No"}</span
                >
              </div>
              ${this._sheetChannel.error
                ? html`
                    <div class="sheet-detail-row">
                      <span class="sheet-detail-label">Error</span>
                      <span class="sheet-detail-value" style="color: var(--hu-error)"
                        >${this._sheetChannel.error}</span
                      >
                    </div>
                  `
                : nothing}
            `
          : nothing}
      </hu-sheet>
    `;
  }

  override render() {
    if (this.loading) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
