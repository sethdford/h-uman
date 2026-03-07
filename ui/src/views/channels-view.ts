import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/sc-data-table-v2.js";
import type { SegmentOption } from "../components/sc-segmented-control.js";
import "../components/sc-badge.js";
import "../components/sc-data-table-v2.js";
import "../components/sc-empty-state.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-sheet.js";
import "../components/sc-skeleton.js";
import "../components/sc-segmented-control.js";
import "../components/sc-stat-card.js";

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

@customElement("sc-channels-view")
export class ScChannelsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-channels;
      display: block;
      max-width: 75rem;
    }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
    }
    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
    .filters {
      margin-bottom: var(--sc-space-lg);
    }
    .table-section {
      margin-top: var(--sc-space-xl);
    }
    .sheet-detail-row {
      display: flex;
      justify-content: space-between;
      padding: var(--sc-space-sm) 0;
      border-bottom: 1px solid var(--sc-border-subtle);
    }
    .sheet-detail-row:last-child {
      border-bottom: none;
    }
    .sheet-detail-label {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .sheet-detail-value {
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
    }
    .sheet-title {
      font-weight: var(--sc-weight-semibold);
      font-size: var(--sc-text-lg);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-lg);
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
        return html`<sc-badge variant=${variant}>${label}</sc-badge>`;
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
      <sc-page-hero role="region" aria-label="Channels overview">
        <sc-section-header
          heading="Channels"
          description="Messaging integrations and their connection status"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
      </div>
      <div class="table-section">
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
      </div>
    `;
  }

  private _renderContent(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Channels overview">
        <sc-section-header
          heading="Channels"
          description="Messaging integrations and their connection status"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-stat-card
          .value=${this.channels.length}
          label="Total Channels"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.channels.filter((ch) => ch.configured === true).length}
          label="Configured"
          style="--sc-stagger-delay: 50ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.channels.filter((ch) => ch.healthy === true).length}
          label="Healthy"
          style="--sc-stagger-delay: 100ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.messagesToday}
          label="Messages Today"
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
      <div class="filters" role="group" aria-label="Filter channels by status">
        <sc-segmented-control
          .options=${this.filterOptions}
          .value=${this.filter}
          @sc-change=${(e: CustomEvent<{ value: string }>) => {
            this.filter = e.detail.value as FilterValue;
          }}
        ></sc-segmented-control>
      </div>
      <div class="table-section" role="region" aria-label="Channels table">
        ${this.filteredChannels.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons.radio}
                heading=${this.filter !== "all" ? "No matching channels" : "No channels configured"}
                description=${this.filter !== "all"
                  ? "Try a different filter."
                  : "Configure messaging channels to receive and send messages."}
              ></sc-empty-state>
            `
          : html`
              <sc-data-table-v2
                .columns=${this.columns}
                .rows=${this.tableRows}
                searchable
                @sc-row-click=${this._onRowClick}
              ></sc-data-table-v2>
            `}
      </div>
      <sc-sheet ?open=${this._sheetChannel != null} @close=${this._onSheetClose}>
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
                      <span class="sheet-detail-value" style="color: var(--sc-error)"
                        >${this._sheetChannel.error}</span
                      >
                    </div>
                  `
                : nothing}
            `
          : nothing}
      </sc-sheet>
    `;
  }

  override render() {
    if (this.loading) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
