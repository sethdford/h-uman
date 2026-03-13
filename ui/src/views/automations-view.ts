import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import "../components/hu-card.js";
import "../components/hu-button.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-metric-row.js";
import "../components/hu-tabs.js";
import "../components/hu-modal.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-automation-card.js";
import "../components/hu-automation-form.js";
import "../components/hu-chart.js";
import { TEMPLATES, cronToHuman } from "../components/hu-automation-form.js";
import type { AutomationFormData } from "../components/hu-automation-form.js";
import { friendlyError } from "../utils/friendly-error.js";

interface CronJob {
  id: number;
  name: string;
  expression: string;
  command: string;
  type: "agent" | "shell";
  channel?: string;
  enabled: boolean;
  paused: boolean;
  one_shot?: boolean;
  next_run: number;
  last_run: number;
  last_status?: string;
  created_at: number;
}

interface CronRun {
  id: number;
  started_at: number;
  finished_at: number;
  status: string;
}

interface ChannelStatus {
  name: string;
  key: string;
  configured: boolean;
}

@customElement("hu-automations-view")
export class ScAutomationsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-automations;
      display: block;
      max-width: 60rem;
      margin: 0 auto;
      font-family: var(--hu-font);
      color: var(--hu-text);
    }

    .job-list {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-lg);
    }

    .modal-footer {
      display: flex;
      justify-content: flex-end;
      gap: var(--hu-space-sm);
      margin-top: var(--hu-space-xl);
      padding-top: var(--hu-space-lg);
      border-top: 1px solid var(--hu-border);
    }

    .delete-body {
      padding: var(--hu-space-lg) 0;
    }

    .delete-body p {
      margin: 0;
      font-size: var(--hu-text-base);
      color: var(--hu-text);
      line-height: var(--hu-leading-relaxed);
    }

    .templates-section {
      margin-bottom: var(--hu-space-xl);
    }

    .templates-section h3 {
      margin: 0 0 var(--hu-space-md);
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .templates-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(12.5rem, 1fr));
      gap: var(--hu-space-md);
    }

    .template-card .template-name {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-xs);
    }

    .template-card .template-desc {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      margin-bottom: var(--hu-space-xs);
    }

    .template-card .template-schedule {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-faint);
    }
    .run-chart-card {
      margin-bottom: var(--hu-space-xl);
    }
    .run-chart-title {
      margin: 0 0 var(--hu-space-md);
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
        transition-duration: 0s !important;
      }
    }
  `;

  @state() private jobs: CronJob[] = [];
  @state() private runsMap = new Map<number, CronRun[]>();
  @state() private channels: ChannelStatus[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private activeTab = "agent";
  @state() private showAgentModal = false;
  @state() private showShellModal = false;
  @state() private editingJob: CronJob | null = null;
  @state() private pendingDelete: CronJob | null = null;
  @state() private selectedTemplate: (typeof TEMPLATES)[number] | null = null;
  @state() private _deleteInProgress = false;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const listRes = await gw.request<{ jobs?: CronJob[] }>("cron.list", {});
      const jobs = listRes?.jobs ?? [];
      this.jobs = Array.isArray(jobs) ? jobs : [];

      const runs = new Map<number, CronRun[]>();
      const runPromises = this.jobs
        .filter((job) => job.id != null)
        .map(async (job) => {
          try {
            const runsRes = await gw.request<{ runs?: CronRun[] }>("cron.runs", {
              id: job.id,
              limit: 100,
            });
            const jobRuns = runsRes?.runs ?? [];
            runs.set(job.id, Array.isArray(jobRuns) ? jobRuns : []);
          } catch {
            runs.set(job.id, []);
          }
        });
      await Promise.all(runPromises);
      this.runsMap = runs;

      try {
        const chRes = await gw.request<{
          channels?: Array<{ key?: string; label?: string; name?: string; configured?: boolean }>;
        }>("channels.status", {});
        const chList = chRes?.channels ?? [];
        this.channels = (Array.isArray(chList) ? chList : [])
          .map((ch) => ({
            name: ch.label ?? ch.name ?? ch.key ?? "",
            key: ch.key ?? ch.label ?? ch.name ?? "",
            configured: ch.configured ?? false,
          }))
          .filter((ch) => ch.key);
      } catch {
        this.channels = [];
      }
    } catch (e) {
      this.error = friendlyError(e);
      this.jobs = [];
      this.runsMap = new Map();
    } finally {
      this.loading = false;
    }
  }

  private get filteredJobs(): CronJob[] {
    return this.jobs.filter((j) => j.type === this.activeTab);
  }

  private get totalCount(): number {
    return this.jobs.length;
  }

  private get activeCount(): number {
    return this.jobs.filter((j) => j.enabled && !j.paused).length;
  }

  private get pausedCount(): number {
    return this.jobs.filter((j) => j.paused).length;
  }

  private get failedCount(): number {
    return this.jobs.filter((j) => j.last_status === "failed").length;
  }

  private get runChartData(): { labels: string[]; datasets: { label: string; data: number[] }[] } {
    const byDay = new Map<string, { success: number; failure: number }>();
    for (const runs of this.runsMap.values()) {
      for (const run of runs) {
        const day = new Date(run.started_at * 1000).toISOString().slice(0, 10);
        const entry = byDay.get(day) ?? { success: 0, failure: 0 };
        if (run.status === "completed") entry.success += 1;
        else entry.failure += 1;
        byDay.set(day, entry);
      }
    }
    const days = [...byDay.keys()].sort();
    return {
      labels: days,
      datasets: [
        { label: "Success", data: days.map((d) => byDay.get(d)!.success) },
        { label: "Failure", data: days.map((d) => byDay.get(d)!.failure) },
      ],
    };
  }

  private _openNewAutomation(): void {
    this.editingJob = null;
    this.selectedTemplate = null;
    if (this.activeTab === "agent") {
      this.showAgentModal = true;
    } else {
      this.showShellModal = true;
    }
  }

  private _handleToggle(e: CustomEvent<{ job: CronJob }>): void {
    const job = e.detail.job;
    const gw = this.gateway;
    if (!gw || job.id == null) return;
    gw.request("cron.update", { id: job.id, enabled: !job.enabled })
      .then(() => this.load())
      .catch((err) => {
        ScToast.show({
          message: err instanceof Error ? err.message : "Failed to update",
          variant: "error",
        });
      });
  }

  private _handleEdit(e: CustomEvent<{ job: CronJob }>): void {
    const job = e.detail.job;
    this.editingJob = job;
    this.selectedTemplate = null;
    if (job.type === "agent") {
      this.showAgentModal = true;
    } else {
      this.showShellModal = true;
    }
  }

  private _useTemplate(t: (typeof TEMPLATES)[number]): void {
    this.editingJob = null;
    this.selectedTemplate = t;
    if (t.type === "agent") {
      this.showAgentModal = true;
    } else {
      this.showShellModal = true;
    }
  }

  private async _handleFormSubmit(e: CustomEvent<AutomationFormData>): Promise<void> {
    const data = e.detail;
    const gw = this.gateway;
    if (!gw) return;
    try {
      if (data.type === "agent") {
        if (this.editingJob) {
          await gw.request("cron.update", {
            id: this.editingJob.id,
            expression: data.expression,
            command: data.prompt,
          });
          ScToast.show({ message: "Automation updated", variant: "success" });
        } else {
          const res = await gw.request<{ id?: number }>("cron.add", {
            type: "agent",
            prompt: data.prompt,
            channel: data.channel,
            expression: data.expression,
            name: data.name,
            one_shot: data.one_shot,
          });
          ScToast.show({ message: "Automation created", variant: "success" });
          if (data.one_shot && res?.id != null) {
            await gw.request("cron.run", { id: res.id });
          }
        }
      } else {
        if (this.editingJob) {
          await gw.request("cron.update", {
            id: this.editingJob.id,
            expression: data.expression,
            command: data.command,
          });
          ScToast.show({ message: "Shell job updated", variant: "success" });
        } else {
          const res = await gw.request<{ id?: number }>("cron.add", {
            expression: data.expression,
            command: data.command!,
            name: data.name,
            one_shot: data.one_shot,
          });
          ScToast.show({ message: "Shell job created", variant: "success" });
          if (data.one_shot && res?.id != null) {
            await gw.request("cron.run", { id: res.id });
          }
        }
      }
      this._closeAgentModal();
      this._closeShellModal();
      this.load();
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to save",
        variant: "error",
      });
    }
  }

  private _handleRun(e: CustomEvent<{ job: CronJob }>): void {
    const job = e.detail.job;
    const gw = this.gateway;
    if (!gw || job.id == null) return;
    gw.request("cron.run", { id: job.id })
      .then(() => {
        ScToast.show({ message: "Automation triggered", variant: "success" });
        this.load();
      })
      .catch((err) => {
        ScToast.show({
          message: err instanceof Error ? err.message : "Failed to run",
          variant: "error",
        });
      });
  }

  private _handleDelete(e: CustomEvent<{ job: CronJob }>): void {
    this.pendingDelete = e.detail.job;
  }

  private async _confirmDelete(): Promise<void> {
    const job = this.pendingDelete;
    if (!job || this._deleteInProgress) return;
    const gw = this.gateway;
    if (!gw || job.id == null) return;
    this._deleteInProgress = true;
    try {
      await gw.request("cron.remove", { id: job.id });
      ScToast.show({ message: "Automation deleted", variant: "success" });
      this.pendingDelete = null;
      this.load();
    } catch (err) {
      ScToast.show({
        message: err instanceof Error ? err.message : "Failed to delete",
        variant: "error",
      });
    } finally {
      this._deleteInProgress = false;
    }
  }

  private _closeAgentModal(): void {
    this.showAgentModal = false;
    this.editingJob = null;
    this.selectedTemplate = null;
  }

  private _closeShellModal(): void {
    this.showShellModal = false;
    this.editingJob = null;
    this.selectedTemplate = null;
  }

  private _renderStats() {
    return html`
      <hu-stats-row>
        <hu-stat-card
          .value=${this.totalCount}
          label="Total"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.activeCount}
          label="Active"
          accent="primary"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.pausedCount}
          label="Paused"
          accent="secondary"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.failedCount}
          label="Failed"
          accent="error"
          style="--hu-stagger-delay: 150ms"
        ></hu-stat-card>
      </hu-stats-row>
      <hu-metric-row
        .items=${[
          {
            label: "Agent Tasks",
            value: String(this.jobs.filter((j) => j.type === "agent").length),
          },
          {
            label: "Shell Jobs",
            value: String(this.jobs.filter((j) => j.type === "shell").length),
          },
          {
            label: "Success Rate",
            value:
              this.jobs.length > 0
                ? Math.round(((this.jobs.length - this.failedCount) / this.jobs.length) * 100) + "%"
                : "—",
          },
        ]}
      ></hu-metric-row>
    `;
  }

  private _renderSkeleton() {
    return html`
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </div>
      <div class="job-list">
        <hu-skeleton variant="card" height="160px"></hu-skeleton>
        <hu-skeleton variant="card" height="160px"></hu-skeleton>
        <hu-skeleton variant="card" height="160px"></hu-skeleton>
      </div>
    `;
  }

  private _renderJobList() {
    if (this.filteredJobs.length === 0) {
      return html`
        <div class="templates-section">
          <h3>Quick Start Templates</h3>
          <div class="templates-grid">
            ${TEMPLATES.filter((t) => t.type === this.activeTab).map(
              (t) => html`
                <hu-card clickable @click=${() => this._useTemplate(t)}>
                  <div class="template-card">
                    <div class="template-name">${t.name}</div>
                    <div class="template-desc">${t.description}</div>
                    <div class="template-schedule">${cronToHuman(t.expression)}</div>
                  </div>
                </hu-card>
              `,
            )}
          </div>
        </div>
        <hu-empty-state
          .icon=${icons.timer}
          heading=${this.activeTab === "agent" ? "No agent tasks" : "No shell jobs"}
          description=${this.activeTab === "agent"
            ? "Create an agent automation to run AI tasks on a schedule."
            : "Create a shell job to run commands on a schedule."}
        >
          <hu-button variant="primary" @click=${this._openNewAutomation}>
            New Automation
          </hu-button>
        </hu-empty-state>
      `;
    }
    return html`
      <div class="job-list">
        ${this.filteredJobs.map(
          (job) => html`
            <hu-automation-card
              .job=${job}
              .runs=${this.runsMap.get(job.id) ?? []}
              @automation-toggle=${this._handleToggle}
              @automation-edit=${this._handleEdit}
              @automation-run=${this._handleRun}
              @automation-delete=${this._handleDelete}
            ></hu-automation-card>
          `,
        )}
      </div>
    `;
  }

  private _renderAgentModal() {
    const template = this.selectedTemplate?.type === "agent" ? this.selectedTemplate : null;
    return html`
      <hu-modal
        heading=${this.editingJob ? "Edit Automation" : "New Agent Automation"}
        ?open=${this.showAgentModal}
        @close=${this._closeAgentModal}
      >
        <hu-automation-form
          type="agent"
          .template=${template}
          .editingJob=${this.editingJob}
          .channels=${this.channels}
          @hu-automation-submit=${this._handleFormSubmit}
          @hu-automation-cancel=${this._closeAgentModal}
        ></hu-automation-form>
      </hu-modal>
    `;
  }

  private _renderShellModal() {
    const template = this.selectedTemplate?.type === "shell" ? this.selectedTemplate : null;
    return html`
      <hu-modal
        heading=${this.editingJob ? "Edit Shell Job" : "New Shell Job"}
        ?open=${this.showShellModal}
        @close=${this._closeShellModal}
      >
        <hu-automation-form
          type="shell"
          .template=${template}
          .editingJob=${this.editingJob}
          @hu-automation-submit=${this._handleFormSubmit}
          @hu-automation-cancel=${this._closeShellModal}
        ></hu-automation-form>
      </hu-modal>
    `;
  }

  private _renderDeleteModal() {
    return html`
      <hu-modal
        heading="Delete Automation?"
        ?open=${!!this.pendingDelete}
        @close=${() => (this.pendingDelete = null)}
      >
        <div class="delete-body">
          <p>
            Are you sure you want to delete
            ${this.pendingDelete ? `'${this.pendingDelete.name || "Unnamed"}'` : ""}? This cannot be
            undone.
          </p>
        </div>
        <div class="modal-footer">
          <hu-button
            variant="secondary"
            ?disabled=${this._deleteInProgress}
            @click=${() => (this.pendingDelete = null)}
          >
            Cancel
          </hu-button>
          <hu-button
            variant="destructive"
            ?disabled=${this._deleteInProgress}
            @click=${this._confirmDelete}
          >
            Delete
          </hu-button>
        </div>
      </hu-modal>
    `;
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
  }

  override render() {
    return html`
      <hu-page-hero role="region" aria-label="Automations">
        <hu-section-header heading="Automations" description="Scheduled agent tasks and shell jobs">
          <hu-button
            variant="primary"
            @click=${this._openNewAutomation}
            aria-label="Create new automation"
            >New Automation</hu-button
          >
        </hu-section-header>
      </hu-page-hero>

      ${this._renderStats()}
      ${this.runChartData.labels.length > 0
        ? html`
            <hu-card class="run-chart-card">
              <h3 class="run-chart-title">Run outcomes over time</h3>
              <hu-chart type="line" .data=${this.runChartData} height=${200}></hu-chart>
            </hu-card>
          `
        : nothing}

      <hu-tabs
        .tabs=${[
          { id: "agent", label: "Agent Tasks" },
          { id: "shell", label: "Shell Jobs" },
        ]}
        .value=${this.activeTab}
        @tab-change=${(e: CustomEvent<string>) => (this.activeTab = e.detail)}
      ></hu-tabs>

      ${this.error
        ? html`<hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <hu-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry loading automations"
              >Retry</hu-button
            >
          </hu-empty-state>`
        : this.loading
          ? this._renderSkeleton()
          : this._renderJobList()}
      ${this._renderAgentModal()} ${this._renderShellModal()} ${this._renderDeleteModal()}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-automations-view": ScAutomationsView;
  }
}
