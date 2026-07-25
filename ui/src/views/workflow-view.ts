import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import { friendlyError } from "../utils/friendly-error.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-empty-state.js";
import "../components/hu-skeleton.js";

interface TaskItem {
  id: number;
  name: string;
  status: string;
  created_at: number;
  updated_at: number;
  parent_task_id?: number;
}

const STATUS_VARIANT: Record<string, string> = {
  pending: "info",
  running: "success",
  completed: "success",
  cancelled: "error",
  failed: "error",
};

@customElement("hu-workflow-view")
export class HuWorkflowView extends GatewayAwareLitElement {
  override autoRefreshInterval = 10_000;

  static override styles = css`
    :host {
      view-transition-name: view-workflow;
      display: block;
      width: 100%;
      min-width: 0;
      color: var(--hu-text);
      contain: layout style;
      container-type: inline-size;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-md), 100%), 1fr));
      gap: var(--hu-space-lg);
      margin-bottom: var(--hu-space-2xl);
    }
    .task-card {
      padding: var(--hu-space-md);
    }
    .task-name {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      margin-bottom: var(--hu-space-xs);
    }
    .task-meta {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
      margin-top: var(--hu-space-xs);
    }
    .task-actions {
      margin-top: var(--hu-space-sm);
    }
  `;

  @state() private tasks: TaskItem[] = [];
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.error = "Not connected to gateway";
      return;
    }
    this.loading = true;
    this.error = "";
    try {
      const res = await gw.request<{ tasks?: TaskItem[] }>("tasks.list", {});
      this.tasks = Array.isArray(res?.tasks) ? res.tasks : [];
    } catch (e) {
      this.error = friendlyError(e);
    } finally {
      this.loading = false;
    }
  }

  private async _cancelTask(id: number): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    try {
      await gw.request("tasks.cancel", { id });
      ScToast.show({ message: `Task ${id} cancelled`, variant: "info" });
      await this.load();
    } catch (e) {
      ScToast.show({ message: friendlyError(e), variant: "error" });
    }
  }

  private _fmtTime(epoch: number): string {
    if (!epoch) return "—";
    return new Date(epoch * 1000).toLocaleString(undefined, {
      dateStyle: "short",
      timeStyle: "short",
    });
  }

  private _renderSkeleton() {
    return html`<div class="grid">
      ${[1, 2, 3].map(() => html`<hu-skeleton style="height:8rem"></hu-skeleton>`)}
    </div>`;
  }

  override render() {
    const hero = html`
      <hu-page-hero role="region" aria-label="Workflows">
        <hu-section-header
          heading="Workflows"
          description="Monitor and manage task execution"
        ></hu-section-header>
      </hu-page-hero>
    `;

    if (this.loading && this.tasks.length === 0) {
      return html`${hero} ${this._renderSkeleton()}`;
    }

    if (this.error) {
      return html`${hero}
        <hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
          <hu-button variant="primary" @click=${() => this.load()}>Retry</hu-button>
        </hu-empty-state>`;
    }

    if (this.tasks.length === 0) {
      return html`${hero}
        <hu-empty-state
          .icon=${icons.clock}
          heading="No tasks"
          description="No workflow tasks have been scheduled yet."
        ></hu-empty-state>`;
    }

    return html`
      ${hero}
      <div class="grid">
        ${this.tasks.map(
          (t) => html`
            <hu-card surface="default" hoverable>
              <div class="task-card">
                <div class="task-name">${t.name}</div>
                <hu-badge variant=${STATUS_VARIANT[t.status] ?? "info"}>${t.status}</hu-badge>
                <div class="task-meta">
                  Created ${this._fmtTime(t.created_at)}<br />
                  Updated ${this._fmtTime(t.updated_at)}
                </div>
                ${
                  t.status === "pending" || t.status === "running"
                    ? html`
                        <div class="task-actions">
                          <hu-button
                            size="sm"
                            variant="outline"
                            @click=${() => this._cancelTask(t.id)}
                          >
                            Cancel
                          </hu-button>
                        </div>
                      `
                    : nothing
                }
              </div>
            </hu-card>
          `,
        )}
      </div>
    `;
  }
}
