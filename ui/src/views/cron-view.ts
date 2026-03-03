import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface CronJob {
  id?: number;
  expression: string;
  command?: string;
  description?: string;
  lastRun?: string;
  status?: string;
}

@customElement("sc-cron-view")
export class ScCronView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-md);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .job-list {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .job-card-inner {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: var(--sc-space-md);
    }
    .job-info {
      flex: 1;
      min-width: 0;
    }
    .job-expression {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-base);
      color: var(--sc-accent);
      margin-bottom: var(--sc-space-xs);
    }
    .job-description {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }
    .job-meta {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .job-actions {
      display: flex;
      gap: var(--sc-space-sm);
      flex-shrink: 0;
    }
    .form-overlay {
      position: fixed;
      inset: 0;
      background: rgba(0, 0, 0, 0.6);
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 1000;
    }
    .form-card {
      padding: 1.5rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      width: 100%;
      max-width: 400px;
    }
    .form-title {
      margin: 0 0 1rem;
      font-size: 1rem;
    }
    .form-group {
      margin-bottom: 1rem;
    }
    .form-group label {
      display: block;
      font-size: 0.75rem;
      color: var(--sc-text-muted);
      margin-bottom: 0.25rem;
    }
    .form-group input {
      width: 100%;
      padding: 0.5rem;
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: 0.875rem;
      box-sizing: border-box;
    }
    .form-actions {
      display: flex;
      gap: 0.5rem;
      justify-content: flex-end;
      margin-top: 1rem;
    }
    .form-title {
      margin: 0 0 var(--sc-space-md);
      font-size: var(--sc-text-lg);
    }
  `;

  @state() private jobs: CronJob[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private showForm = false;
  @state() private formExpression = "";
  @state() private formCommand = "";
  @state() private formDescription = "";

  protected override async load(): Promise<void> {
    await this.loadJobs();
  }

  private async loadJobs(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const res = (await gw.request<{ jobs?: CronJob[] }>("cron.list", {})) as
        | { jobs?: CronJob[] }
        | { result?: { jobs?: CronJob[] } };
      const jobs =
        (res && "jobs" in res && res.jobs) || (res && "result" in res && res.result?.jobs) || [];
      this.jobs = Array.isArray(jobs) ? jobs : [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load jobs";
      this.jobs = [];
    } finally {
      this.loading = false;
    }
  }

  private openAddForm(): void {
    this.formExpression = "";
    this.formCommand = "";
    this.formDescription = "";
    this.showForm = true;
  }

  private closeForm(): void {
    this.showForm = false;
  }

  private async submitAdd(): Promise<void> {
    const gw = this.gateway;
    if (!gw || !this.formExpression.trim() || !this.formCommand.trim()) return;
    this.error = "";
    try {
      await gw.request("cron.add", {
        expression: this.formExpression.trim(),
        command: this.formCommand.trim(),
        name: this.formDescription.trim() || undefined,
      });
      this.closeForm();
      await this.loadJobs();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to add job";
    }
  }

  private async runJob(job: CronJob): Promise<void> {
    const gw = this.gateway;
    if (!gw || job.id == null) {
      this.error = "Cannot run job without a numeric ID";
      return;
    }
    try {
      await gw.request("cron.run", { id: job.id });
      await this.loadJobs();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to run job";
    }
  }

  private async deleteJob(job: CronJob): Promise<void> {
    const gw = this.gateway;
    if (!gw || job.id == null) {
      this.error = "Cannot delete job without a numeric ID";
      return;
    }
    try {
      await gw.request("cron.remove", { id: job.id });
      await this.loadJobs();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to delete job";
    }
  }

  override render() {
    return html`
      <div class="header">
        <h2>Cron Jobs</h2>
        <sc-button variant="primary" @click=${this.openAddForm}>Add Job</sc-button>
      </div>
      ${this.error
        ? html`<sc-empty-state
            icon="⚠️"
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading
        ? html`
            <div class="job-list">
              <sc-skeleton variant="card" height="80px"></sc-skeleton>
              <sc-skeleton variant="card" height="80px"></sc-skeleton>
            </div>
          `
        : this.jobs.length === 0
          ? html`
              <sc-empty-state
                icon="⏰"
                heading="No scheduled jobs"
                description="Add a cron job to run commands on a schedule."
              ></sc-empty-state>
            `
          : html`
              <div class="job-list">
                ${this.jobs.map(
                  (job) => html`
                    <sc-card>
                      <div class="job-card-inner">
                        <div class="job-info">
                          <div class="job-expression">${job.expression}</div>
                          ${
                            job.command
                              ? html`<div class="job-description">
                                  <code>${job.command}</code>
                                </div>`
                              : ""
                          }
                          ${
                            job.description
                              ? html`<div class="job-description">${job.description}</div>`
                              : ""
                          }
                          <div class="job-meta">
                            ${job.lastRun ? `Last run: ${job.lastRun}` : ""}
                            ${job.status ? ` · ${job.status}` : ""}
                          </div>
                        </div>
                        <div class="job-actions">
                          <sc-button
                            variant="secondary"
                            ?disabled=${job.id == null}
                            @click=${() => this.runJob(job)}
                          >
                            Run Now
                          </sc-button>
                          <sc-button
                            variant="destructive"
                            ?disabled=${job.id == null}
                            @click=${() => this.deleteJob(job)}
                          >
                            Delete
                          </sc-button>
                        </div>
                      </div>
                    </sc-card>
                    </sc-card>
                  `,
                )}
              </div>
            `}
      ${this.showForm
        ? html`
            <div class="form-overlay" @click=${this.closeForm}>
              <div class="form-card" @click=${(e: Event) => e.stopPropagation()}>
                <h3 class="form-title">Add Cron Job</h3>
                <div class="form-group">
                  <label>Expression (e.g. 0 * * * *)</label>
                  <input
                    type="text"
                    placeholder="0 * * * *"
                    .value=${this.formExpression}
                    @input=${(e: Event) =>
                      (this.formExpression = (e.target as HTMLInputElement).value)}
                  />
                </div>
                <div class="form-group">
                  <label>Command (shell command to run)</label>
                  <input
                    type="text"
                    placeholder="echo 'hello world'"
                    .value=${this.formCommand}
                    @input=${(e: Event) =>
                      (this.formCommand = (e.target as HTMLInputElement).value)}
                  />
                </div>
                <div class="form-group">
                  <label>Description (optional)</label>
                  <input
                    type="text"
                    placeholder="Optional"
                    .value=${this.formDescription}
                    @input=${(e: Event) =>
                      (this.formDescription = (e.target as HTMLInputElement).value)}
                  />
                </div>
                <div class="form-actions">
                  <sc-button variant="secondary" @click=${this.closeForm}>Cancel</sc-button>
                  <sc-button variant="primary" @click=${this.submitAdd}>Add</sc-button>
                </div>
              </div>
            </div>
          `
        : ""}
    `;
  }
}
