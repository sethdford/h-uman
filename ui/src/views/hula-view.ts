import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { staggerMotion9Styles } from "../styles/scroll-entrance.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-card.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import "../components/hu-stat-card.js";
import "../components/hu-hula-tree.js";
import { friendlyError } from "../utils/friendly-error.js";

interface HulaTraceListItem {
  id?: string;
  size?: number;
  mtime?: number;
}

interface HulaTraceRecord {
  id?: string;
  /** Present when `trace_limit` / `trace_offset` were sent (gateway slices `record.trace`). */
  trace_truncated?: boolean;
  trace_total_steps?: number;
  trace_returned_count?: number;
  trace_offset?: number;
  trace_limit?: number;
  record?: {
    program_name?: string;
    program?: unknown;
    program_source?: string;
    success?: boolean;
    trace?: Record<string, unknown>[];
  };
}

interface HulaAnalyticsSummary {
  file_count?: number;
  success_count?: number;
  fail_count?: number;
  total_trace_steps?: number;
  newest_ts?: number;
}

@customElement("hu-hula-view")
export class ScHulaView extends GatewayAwareLitElement {
  override autoRefreshInterval = 12_000;

  private readonly _visHandler = (): void => {
    if (document.visibilityState === "visible") void this.load();
  };

  override connectedCallback(): void {
    super.connectedCallback();
    document.addEventListener("visibilitychange", this._visHandler);
  }

  override disconnectedCallback(): void {
    document.removeEventListener("visibilitychange", this._visHandler);
    super.disconnectedCallback();
  }

  static override styles = [
    staggerMotion9Styles,
    css`
      :host {
        view-transition-name: view-hula;
        display: block;
        width: 100%;
        min-width: 0;
        max-width: 72rem;
        color: var(--hu-text);
        container-type: inline-size;
      }
      .icon-inline {
        display: inline-flex;
        width: 1em;
        height: 1em;
        color: var(--hu-accent);
      }
      .layout {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(0, 1.2fr);
        gap: var(--hu-space-lg);
        align-items: start;
      }
      .trace-list {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-xs);
      }
      .trace-row {
        display: flex;
        justify-content: space-between;
        align-items: center;
        gap: var(--hu-space-md);
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-radius: var(--hu-radius);
        background: var(--hu-surface-container);
        cursor: pointer;
        border: 1px solid transparent;
        text-align: left;
        width: 100%;
        font-family: var(--hu-font);
        color: var(--hu-text);
        transition:
          border-color var(--hu-duration-fast) var(--hu-ease-out),
          background var(--hu-duration-fast) var(--hu-ease-out);
      }
      .trace-row:hover {
        border-color: var(--hu-border-strong);
      }
      .trace-row:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      .trace-row[aria-current="true"] {
        background: var(--hu-surface-container-high);
        border-color: var(--hu-accent);
      }
      .trace-id {
        font-family: var(--hu-font-mono);
        font-size: var(--hu-text-sm);
      }
      .trace-meta {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
      }
      .detail-head {
        display: flex;
        justify-content: space-between;
        align-items: center;
        gap: var(--hu-space-md);
        margin-bottom: var(--hu-space-md);
        flex-wrap: wrap;
      }
      .error-banner {
        padding: var(--hu-space-md);
        border-radius: var(--hu-radius);
        background: color-mix(in srgb, var(--hu-error) 12%, transparent);
        color: var(--hu-text);
        margin-bottom: var(--hu-space-lg);
      }
      .detail-error {
        padding: var(--hu-space-md);
        border-radius: var(--hu-radius);
        background: color-mix(in srgb, var(--hu-error) 10%, transparent);
        color: var(--hu-text);
        font-size: var(--hu-text-sm);
        margin-bottom: var(--hu-space-md);
      }
      .trace-window-note {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
        margin-bottom: var(--hu-space-md);
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-radius: var(--hu-radius);
        background: var(--hu-surface-container);
      }
      .stats {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(9rem, 100%), 1fr));
        gap: var(--hu-space-md);
        margin-bottom: var(--hu-space-xl);
      }
      @container (max-width: 52rem) /* cq-expanded */ {
        .layout {
          grid-template-columns: 1fr;
        }
      }
      @media (prefers-reduced-motion: reduce) {
        * {
          animation-duration: 0s !important;
          transition-duration: 0s !important;
        }
      }
    `,
  ];

  @state() private traces: HulaTraceListItem[] = [];
  @state() private directory = "";
  @state() private selected: HulaTraceListItem | null = null;
  @state() private detail: HulaTraceRecord | null = null;
  @state() private analytics: HulaAnalyticsSummary | null = null;
  @state() private loading = true;
  @state() private listError = "";
  @state() private detailLoading = false;
  @state() private detailError = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.listError = "Not connected to gateway";
      this.loading = false;
      return;
    }
    this.loading = true;
    this.listError = "";
    try {
      const [listRes, anRes] = await Promise.all([
        gw.request<{ traces?: HulaTraceListItem[]; directory?: string }>("hula.traces.list", {}),
        gw.request<{ summary?: HulaAnalyticsSummary }>("hula.traces.analytics", {}),
      ]);
      this.traces = listRes.traces ?? [];
      this.directory = listRes.directory ?? "";
      this.analytics = anRes.summary ?? null;
      if (!this.selected && this.traces.length) {
        this.selected = this.traces[0] ?? null;
        if (this.selected?.id) await this._loadDetail(this.selected.id);
      } else if (this.selected?.id) {
        await this._loadDetail(this.selected.id);
      }
    } catch (e) {
      this.listError = friendlyError(e);
    } finally {
      this.loading = false;
    }
  }

  private async _loadDetail(id: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.detailLoading = true;
    this.detailError = "";
    try {
      const res = await gw.request<HulaTraceRecord>("hula.traces.get", {
        id,
        trace_limit: 500,
        trace_offset: 0,
      });
      this.detail = res;
    } catch (e) {
      this.detailError = friendlyError(e);
      this.detail = null;
    } finally {
      this.detailLoading = false;
    }
  }

  private async _onSelect(t: HulaTraceListItem): Promise<void> {
    this.selected = t;
    this.detailError = "";
    if (t.id) await this._loadDetail(t.id);
  }

  private async _onDelete(): Promise<void> {
    if (!this.selected?.id) return;
    const gw = this.gateway;
    if (!gw) return;
    try {
      await gw.request("hula.traces.delete", { id: this.selected.id });
      this.detail = null;
      this.selected = null;
      this.detailError = "";
      await this.load();
    } catch (e) {
      this.listError = friendlyError(e);
    }
  }

  private async _onRefresh(): Promise<void> {
    this.detailError = "";
    await this.load();
  }

  private async _copyProgramJson(): Promise<void> {
    const rec = this.detail?.record;
    if (rec?.program === undefined) {
      this.detailError = "No embedded program in this trace.";
      return;
    }
    try {
      const text =
        typeof rec.program === "string" ? rec.program : JSON.stringify(rec.program, null, 2);
      await navigator.clipboard.writeText(text);
    } catch (e) {
      this.detailError = friendlyError(e);
    }
  }

  private async _copyProgramSource(): Promise<void> {
    const s = this.detail?.record?.program_source;
    if (!s) {
      this.detailError = "No program_source field in this trace.";
      return;
    }
    try {
      await navigator.clipboard.writeText(s);
    } catch (e) {
      this.detailError = friendlyError(e);
    }
  }

  override render() {
    return html`
      <hu-page-hero role="region" aria-label="HuLa traces">
        <hu-section-header
          heading="HuLa traces"
          description="Browse persisted orchestration traces and step-level execution trees"
        >
          <span class="icon-inline" aria-hidden="true">${icons.code}</span>
        </hu-section-header>
      </hu-page-hero>

      ${
        this.listError
          ? html`<div class="error-banner" role="alert">${this.listError}</div>`
          : nothing
      }
      ${
        this.loading
          ? html`<hu-skeleton style="height:6rem;border-radius:var(--hu-radius)"></hu-skeleton>`
          : nothing
      }
      ${
        !this.loading && this.analytics
          ? html`
              <div class="stats">
                <hu-stat-card
                  label="Trace files"
                  .value=${this.analytics.file_count ?? 0}
                ></hu-stat-card>
                <hu-stat-card
                  label="Successful runs"
                  .value=${this.analytics.success_count ?? 0}
                ></hu-stat-card>
                <hu-stat-card
                  label="Failed runs"
                  .value=${this.analytics.fail_count ?? 0}
                ></hu-stat-card>
                <hu-stat-card
                  label="Trace steps (total)"
                  .value=${this.analytics.total_trace_steps ?? 0}
                ></hu-stat-card>
              </div>
            `
          : nothing
      }

      <div class="layout">
        <hu-card>
          <hu-section-header heading="Saved traces"></hu-section-header>
          ${
            !this.loading && !this.traces.length
              ? html`<hu-empty-state
                  heading="No traces yet"
                  description="Run a HuLa program with HU_HULA_TRACE_DIR set (or use the default ~/.human/hula_traces on POSIX). The gateway lists traces on POSIX builds only."
                ></hu-empty-state>`
              : nothing
          }
          <div class="trace-list hu-stagger-motion9" role="list">
            ${this.traces.map(
              (t) => html`
                <button
                  type="button"
                  class="trace-row"
                  role="listitem"
                  aria-current=${this.selected?.id === t.id ? "true" : "false"}
                  @click=${() => this._onSelect(t)}
                >
                  <span class="trace-id">${t.id ?? "?"}</span>
                  <span class="trace-meta"
                    >${t.size != null ? `${t.size} B` : ""}${
                      t.mtime != null
                        ? ` · ${new Date((t.mtime as number) * 1000).toLocaleString()}`
                        : ""
                    }</span
                  >
                </button>
              `,
            )}
          </div>
          ${
            this.directory
              ? html`<p
                  style="font-size:var(--hu-text-xs);color:var(--hu-text-muted);margin-top:var(--hu-space-md);"
                >
                  ${this.directory}
                </p>`
              : nothing
          }
        </hu-card>

        <hu-card>
          <div class="detail-head">
            <hu-section-header heading="Trace detail"></hu-section-header>
            <div style="display:flex;flex-wrap:wrap;gap:var(--hu-space-sm);align-items:center;">
              <hu-button
                variant="secondary"
                ?disabled=${this.loading}
                @click=${() => this._onRefresh()}
                >Refresh</hu-button
              >
              <hu-button
                variant="secondary"
                ?disabled=${!this.detail?.record || this.detailLoading}
                @click=${() => this._copyProgramJson()}
                >Copy program</hu-button
              >
              <hu-button
                variant="secondary"
                ?disabled=${!this.detail?.record?.program_source || this.detailLoading}
                @click=${() => this._copyProgramSource()}
                >Copy source</hu-button
              >
              <hu-button
                variant="secondary"
                ?disabled=${!this.selected?.id || this.detailLoading}
                @click=${() => this._onDelete()}
                >Delete</hu-button
              >
            </div>
          </div>
          ${
            this.detailError
              ? html`<div class="detail-error" role="alert">${this.detailError}</div>`
              : nothing
          }
          ${
            this.detailLoading
              ? html`<hu-skeleton
                  style="height:12rem;border-radius:var(--hu-radius)"
                ></hu-skeleton>`
              : this.detail?.record
                ? html`
                    <p style="font-size:var(--hu-text-sm);margin-bottom:var(--hu-space-md);">
                      <strong>${this.detail.record.program_name ?? "program"}</strong>
                      · ${this.detail.record.success ? "success" : "failed"}
                    </p>
                    ${
                      this.detail.trace_truncated
                        ? html`<p class="trace-window-note" role="status">
                            Showing ${this.detail.trace_returned_count ?? "—"} of
                            ${this.detail.trace_total_steps ?? "—"} steps (offset
                            ${this.detail.trace_offset ?? 0}; limit
                            ${this.detail.trace_limit ?? "—"}). Re-fetch with RPC params to page.
                          </p>`
                        : nothing
                    }
                    <hu-hula-tree .steps=${this.detail.record.trace ?? []}></hu-hula-tree>
                  `
                : this.selected?.id
                  ? html`<hu-empty-state
                      heading="Could not load trace"
                      description="The list entry is selected but no record was returned. Try another file or refresh."
                    ></hu-empty-state>`
                  : html`<hu-empty-state
                      heading="Select a trace"
                      description="Choose a file on the left to inspect its execution tree."
                    ></hu-empty-state>`
          }
        </hu-card>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-hula-view": ScHulaView;
  }
}
