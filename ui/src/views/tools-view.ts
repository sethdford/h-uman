import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import type { DataTableColumnV2 } from "../components/hu-data-table-v2.js";
import "../components/hu-button.js";
import "../components/hu-data-table-v2.js";
import "../components/hu-json-viewer.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";

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

interface ToolDef {
  name?: string;
  description?: string;
  parameters?: unknown;
}

function resolveParams(raw: unknown): Record<string, unknown> | null {
  if (raw == null) return null;
  if (typeof raw === "string") {
    try {
      const parsed = JSON.parse(raw);
      if (typeof parsed === "object" && parsed !== null) return parsed as Record<string, unknown>;
    } catch {
      return null;
    }
    return null;
  }
  if (typeof raw === "object") return raw as Record<string, unknown>;
  return null;
}

function countParams(raw: unknown): number {
  const obj = resolveParams(raw);
  if (!obj) return 0;
  const props = obj.properties;
  if (props && typeof props === "object" && !Array.isArray(props))
    return Object.keys(props as object).length;
  return Object.keys(obj).length;
}

@customElement("hu-tools-view")
export class ScToolsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      view-transition-name: view-tools;
      display: block;
      max-width: 75rem;
    }
    .table-section {
      margin-top: var(--hu-space-xl);
    }
    .expand-panel {
      margin-top: var(--hu-space-md);
      padding: var(--hu-space-md);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
    }
    .expand-panel-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--hu-space-md);
      font-weight: var(--hu-weight-semibold);
      font-size: var(--hu-text-base);
      color: var(--hu-text);
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
    }
  `;

  @state() private tools: ToolDef[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private _expandedTool: string | null = null;

  protected override async load(): Promise<void> {
    await this.loadTools();
  }

  private async loadTools(): Promise<void> {
    if (!this.gateway) {
      this.loading = false;
      return;
    }
    this.loading = true;
    try {
      const payload = await this.gateway.request<{ tools?: ToolDef[] }>("tools.catalog", {});
      this.tools = payload?.tools ?? [];
    } catch (e) {
      this.tools = [];
      this.error = friendlyError(e);
    } finally {
      this.loading = false;
    }
  }

  private get tableRows(): Record<string, unknown>[] {
    return this.tools.map((t) => ({
      name: t.name ?? "unnamed",
      description: t.description ?? "",
      paramsCount: countParams(t.parameters),
      parameters: resolveParams(t.parameters) ?? t.parameters,
    }));
  }

  private readonly columns: DataTableColumnV2[] = [
    { key: "name", label: "Name", sortable: true },
    { key: "description", label: "Description" },
    {
      key: "paramsCount",
      label: "Parameters",
      align: "right",
      sortable: true,
      render: (v) => (v != null && typeof v === "number" ? String(v) : "0"),
    },
  ];

  private _onRowClick(e: CustomEvent<{ row: Record<string, unknown>; index: number }>): void {
    const name = String(e.detail.row.name ?? "");
    this._expandedTool = this._expandedTool === name ? null : name;
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  private get _expandedToolData(): unknown {
    if (!this._expandedTool) return undefined;
    const t = this.tools.find((x) => (x.name ?? "unnamed") === this._expandedTool);
    return t?.parameters;
  }

  private _renderSkeleton() {
    return html`
      <hu-page-hero role="region" aria-label="Tools overview">
        <hu-section-header
          heading="Tools"
          description="Available tool integrations and their configurations"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <div class="table-section">
        <hu-skeleton variant="card" height="200px"></hu-skeleton>
      </div>
    `;
  }

  private _renderContent() {
    const rows = this.tableRows;
    const count = this.tools.length;
    const expandedParams = this._expandedToolData;

    return html`
      <hu-page-hero role="region" aria-label="Tools overview">
        <hu-section-header
          heading="Tools"
          description="Available tool integrations and their configurations"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-stat-card
          .value=${count}
          label="Total Tools"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.tools.filter((t) => countParams(t.parameters) > 0).length}
          label="With Parameters"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${rows.reduce((sum, r) => sum + (Number(r.paramsCount) || 0), 0)}
          label="Total Parameters"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
      </hu-stats-row>
      ${this.error
        ? html`<hu-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></hu-empty-state>`
        : nothing}
      <div class="table-section" role="region" aria-label="Tools table">
        ${rows.length === 0
          ? html`
              <hu-empty-state
                .icon=${icons.wrench}
                heading="No tools available"
                description="Tools will appear here when the gateway provides them."
              ></hu-empty-state>
            `
          : html`
              <hu-data-table-v2
                .columns=${this.columns}
                .rows=${rows}
                searchable
                @hu-row-click=${this._onRowClick}
              ></hu-data-table-v2>
              ${expandedParams !== undefined
                ? html`
                    <div class="expand-panel" role="region" aria-label="Parameter schema">
                      <div class="expand-panel-header">
                        <span>Parameters: ${this._expandedTool}</span>
                        <hu-button
                          variant="ghost"
                          size="sm"
                          @click=${() => {
                            this._expandedTool = null;
                          }}
                          aria-label="Close parameter view"
                        >
                          Close
                        </hu-button>
                      </div>
                      <hu-json-viewer
                        .data=${expandedParams}
                        root-label="parameters"
                      ></hu-json-viewer>
                    </div>
                  `
                : nothing}
            `}
      </div>
    `;
  }

  override render() {
    if (this.loading) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
