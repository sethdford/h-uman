import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-card.js";
import "../components/sc-button.js";
import "../components/sc-tabs.js";
import "../components/sc-modal.js";
import "../components/sc-input.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-automation-card.js";
import "../components/sc-schedule-builder.js";

interface CronJob {
  id: number;
  name: string;
  expression: string;
  command: string;
  type: "agent" | "shell";
  channel?: string;
  enabled: boolean;
  paused: boolean;
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

@customElement("sc-automations-view")
export class ScAutomationsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      max-width: 960px;
      margin: 0 auto;
      font-family: var(--sc-font);
      color: var(--sc-text);
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }

    .header-title {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }

    .header-title .icon {
      width: 24px;
      height: 24px;
      color: var(--sc-text-muted);
    }

    .stats-bar {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }

    .stat-card {
      flex: 1;
      min-width: 100px;
      padding: var(--sc-space-md) var(--sc-space-lg);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-xs);
    }

    .stat-value {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      font-variant-numeric: tabular-nums;
    }

    .stat-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-top: var(--sc-space-2xs);
    }

    .job-list {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-lg);
    }

    .form-group {
      margin-bottom: var(--sc-space-md);
    }

    .form-group label {
      display: block;
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }

    .form-textarea {
      width: 100%;
      box-sizing: border-box;
      min-height: 100px;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      resize: vertical;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }

    .form-textarea::placeholder {
      color: var(--sc-text-muted);
    }

    .form-textarea:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }

    .form-textarea:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .form-select {
      width: 100%;
      box-sizing: border-box;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .form-select:hover:not(:disabled) {
      border-color: var(--sc-text-faint);
    }

    .form-select:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .form-error {
      font-size: var(--sc-text-sm);
      color: var(--sc-error);
      margin-top: var(--sc-space-xs);
    }

    .modal-footer {
      display: flex;
      justify-content: flex-end;
      gap: var(--sc-space-sm);
      margin-top: var(--sc-space-xl);
      padding-top: var(--sc-space-lg);
      border-top: 1px solid var(--sc-border);
    }

    .delete-body {
      padding: var(--sc-space-lg) 0;
    }

    .delete-body p {
      margin: 0;
      font-size: var(--sc-text-base);
      color: var(--sc-text);
      line-height: var(--sc-leading-relaxed);
    }

    .mono-input :host(sc-input) input,
    .mono-input input {
      font-family: var(--sc-font-mono) !important;
    }

    @media (max-width: 640px) {
      .stats-bar {
        flex-direction: column;
      }

      .stat-card {
        min-width: 100%;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .form-textarea,
      .form-select {
        transition: none;
      }
    }
  `;

  @state() private jobs: CronJob[] = [];
  @state() private runsMap = new Map<number, CronRun[]>();
  @state() private channels: ChannelStatus[] = [];
  @state() private loading = false;
  @state() private activeTab = "agent";
  @state() private showAgentModal = false;
  @state() private showShellModal = false;
  @state() private editingJob: CronJob | null = null;
  @state() private pendingDelete: CronJob | null = null;
  @state() private agentPrompt = "";
  @state() private agentChannel = "";
  @state() private agentSchedule = "";
  @state() private agentName = "";
  @state() private shellCommand = "";
  @state() private shellSchedule = "";
  @state() private shellName = "";
  @state() private formError = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    try {
      const listRes = await gw.request<{ jobs?: CronJob[] }>("cron.list", {});
      const jobs = listRes?.jobs ?? [];
      this.jobs = Array.isArray(jobs) ? jobs : [];

      const runs = new Map<number, CronRun[]>();
      for (const job of this.jobs) {
        if (job.id == null) continue;
        try {
          const runsRes = await gw.request<{ runs?: CronRun[] }>("cron.runs", {
            id: job.id,
            limit: 7,
          });
          const jobRuns = runsRes?.runs ?? [];
          runs.set(job.id, Array.isArray(jobRuns) ? jobRuns : []);
        } catch {
          runs.set(job.id, []);
        }
      }
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
    } catch {
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

  private _openNewAutomation(): void {
    this._resetForm();
    this.editingJob = null;
    if (this.activeTab === "agent") {
      this.showAgentModal = true;
    } else {
      this.showShellModal = true;
    }
  }

  private _resetForm(): void {
    this.agentPrompt = "";
    this.agentChannel = "";
    this.agentSchedule = "0 8 * * *";
    this.agentName = "";
    this.shellCommand = "";
    this.shellSchedule = "0 8 * * *";
    this.shellName = "";
    this.formError = "";
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
    this.formError = "";
    if (job.type === "agent") {
      this.agentName = job.name ?? "";
      this.agentPrompt = job.command ?? "";
      this.agentChannel = job.channel ?? "";
      this.agentSchedule = job.expression ?? "0 8 * * *";
      this.showAgentModal = true;
    } else {
      this.shellName = job.name ?? "";
      this.shellCommand = job.command ?? "";
      this.shellSchedule = job.expression ?? "0 8 * * *";
      this.showShellModal = true;
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
    if (!job) return;
    const gw = this.gateway;
    if (!gw || job.id == null) return;
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
    }
  }

  private _closeAgentModal(): void {
    this.showAgentModal = false;
    this.editingJob = null;
  }

  private _closeShellModal(): void {
    this.showShellModal = false;
    this.editingJob = null;
  }

  private async _saveAgent(): Promise<void> {
    const prompt = this.agentPrompt.trim();
    const schedule = this.agentSchedule.trim();
    if (!prompt) {
      this.formError = "Prompt is required";
      return;
    }
    if (!schedule) {
      this.formError = "Schedule is required";
      return;
    }
    const gw = this.gateway;
    if (!gw) return;
    this.formError = "";
    try {
      if (this.editingJob) {
        await gw.request("cron.update", {
          id: this.editingJob.id,
          expression: schedule,
          command: prompt,
        });
        ScToast.show({ message: "Automation updated", variant: "success" });
      } else {
        await gw.request("cron.add", {
          type: "agent",
          prompt,
          channel: this.agentChannel || undefined,
          expression: schedule,
          name: this.agentName.trim() || "Agent task",
        });
        ScToast.show({ message: "Automation created", variant: "success" });
      }
      this._closeAgentModal();
      this.load();
    } catch (err) {
      this.formError = err instanceof Error ? err.message : "Failed to save";
    }
  }

  private async _saveShell(): Promise<void> {
    const cmd = this.shellCommand.trim();
    const schedule = this.shellSchedule.trim();
    if (!cmd) {
      this.formError = "Command is required";
      return;
    }
    if (!schedule) {
      this.formError = "Schedule is required";
      return;
    }
    const gw = this.gateway;
    if (!gw) return;
    this.formError = "";
    try {
      if (this.editingJob) {
        await gw.request("cron.update", {
          id: this.editingJob.id,
          expression: schedule,
          command: cmd,
        });
        ScToast.show({ message: "Shell job updated", variant: "success" });
      } else {
        await gw.request("cron.add", {
          expression: schedule,
          command: cmd,
          name: this.shellName.trim() || cmd,
        });
        ScToast.show({ message: "Shell job created", variant: "success" });
      }
      this._closeShellModal();
      this.load();
    } catch (err) {
      this.formError = err instanceof Error ? err.message : "Failed to save";
    }
  }

  override render() {
    return html`
      <div class="header">
        <h1 class="header-title">
          <span class="icon" aria-hidden="true">${icons.timer}</span>
          Automations
        </h1>
        <sc-button variant="primary" @click=${this._openNewAutomation}> New Automation </sc-button>
      </div>

      <div class="stats-bar">
        <div class="stat-card">
          <div class="stat-value">${this.totalCount}</div>
          <div class="stat-label">Total</div>
        </div>
        <div class="stat-card">
          <div class="stat-value">${this.activeCount}</div>
          <div class="stat-label">Active</div>
        </div>
        <div class="stat-card">
          <div class="stat-value">${this.pausedCount}</div>
          <div class="stat-label">Paused</div>
        </div>
        <div class="stat-card">
          <div class="stat-value">${this.failedCount}</div>
          <div class="stat-label">Failed</div>
        </div>
      </div>

      <sc-tabs
        .tabs=${[
          { id: "agent", label: "Agent Tasks" },
          { id: "shell", label: "Shell Jobs" },
        ]}
        .value=${this.activeTab}
        @tab-change=${(e: CustomEvent<string>) => (this.activeTab = e.detail)}
      ></sc-tabs>

      ${this.loading
        ? html`
            <div class="job-list">
              <sc-skeleton variant="card" height="160px"></sc-skeleton>
              <sc-skeleton variant="card" height="160px"></sc-skeleton>
              <sc-skeleton variant="card" height="160px"></sc-skeleton>
            </div>
          `
        : this.filteredJobs.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons.timer}
                heading=${this.activeTab === "agent" ? "No agent tasks" : "No shell jobs"}
                description=${this.activeTab === "agent"
                  ? "Create an agent automation to run AI tasks on a schedule."
                  : "Create a shell job to run commands on a schedule."}
              >
                <sc-button variant="primary" @click=${this._openNewAutomation}>
                  New Automation
                </sc-button>
              </sc-empty-state>
            `
          : html`
              <div class="job-list">
                ${this.filteredJobs.map(
                  (job) => html`
                    <sc-automation-card
                      .job=${job}
                      .runs=${this.runsMap.get(job.id) ?? []}
                      @automation-toggle=${this._handleToggle}
                      @automation-edit=${this._handleEdit}
                      @automation-run=${this._handleRun}
                      @automation-delete=${this._handleDelete}
                    ></sc-automation-card>
                  `,
                )}
              </div>
            `}

      <sc-modal
        heading=${this.editingJob ? "Edit Automation" : "New Agent Automation"}
        ?open=${this.showAgentModal}
        @close=${this._closeAgentModal}
      >
        <div class="form-group">
          <sc-input
            label="Name"
            placeholder="My daily summary"
            .value=${this.agentName}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.agentName = e.detail.value)}
          ></sc-input>
        </div>
        <div class="form-group">
          <label for="agent-prompt">Prompt</label>
          <textarea
            id="agent-prompt"
            class="form-textarea"
            placeholder="Summarize my unread messages..."
            .value=${this.agentPrompt}
            @input=${(e: Event) => (this.agentPrompt = (e.target as HTMLTextAreaElement).value)}
          ></textarea>
        </div>
        <div class="form-group">
          <label for="agent-channel">Channel</label>
          <select
            id="agent-channel"
            class="form-select"
            .value=${this.agentChannel}
            @change=${(e: Event) => (this.agentChannel = (e.target as HTMLSelectElement).value)}
          >
            <option value="">— Gateway (default) —</option>
            ${this.channels.map(
              (ch) =>
                html`<option value=${ch.name} ?disabled=${!ch.configured}>${ch.name}</option>`,
            )}
          </select>
        </div>
        <div class="form-group">
          <label>Schedule</label>
          <sc-schedule-builder
            .value=${this.agentSchedule}
            @sc-schedule-change=${(e: CustomEvent<{ value: string }>) =>
              (this.agentSchedule = e.detail.value)}
          ></sc-schedule-builder>
        </div>
        ${this.formError ? html`<p class="form-error">${this.formError}</p>` : nothing}
        <div class="modal-footer">
          <sc-button variant="secondary" @click=${this._closeAgentModal}>Cancel</sc-button>
          <sc-button variant="primary" @click=${this._saveAgent}>Save</sc-button>
        </div>
      </sc-modal>

      <sc-modal
        heading=${this.editingJob ? "Edit Shell Job" : "New Shell Job"}
        ?open=${this.showShellModal}
        @close=${this._closeShellModal}
      >
        <div class="form-group">
          <sc-input
            label="Name"
            placeholder="My backup script"
            .value=${this.shellName}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.shellName = e.detail.value)}
          ></sc-input>
        </div>
        <div class="form-group mono-input">
          <sc-input
            label="Command"
            placeholder="echo 'hello world'"
            .value=${this.shellCommand}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.shellCommand = e.detail.value)}
          ></sc-input>
        </div>
        <div class="form-group">
          <label>Schedule</label>
          <sc-schedule-builder
            .value=${this.shellSchedule}
            @sc-schedule-change=${(e: CustomEvent<{ value: string }>) =>
              (this.shellSchedule = e.detail.value)}
          ></sc-schedule-builder>
        </div>
        ${this.formError ? html`<p class="form-error">${this.formError}</p>` : nothing}
        <div class="modal-footer">
          <sc-button variant="secondary" @click=${this._closeShellModal}>Cancel</sc-button>
          <sc-button variant="primary" @click=${this._saveShell}>Save</sc-button>
        </div>
      </sc-modal>

      <sc-modal
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
          <sc-button variant="secondary" @click=${() => (this.pendingDelete = null)}>
            Cancel
          </sc-button>
          <sc-button variant="destructive" @click=${this._confirmDelete}> Delete </sc-button>
        </div>
      </sc-modal>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-automations-view": ScAutomationsView;
  }
}
