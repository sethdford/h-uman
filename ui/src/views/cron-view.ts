import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface CronJob {
  id?: number;
  expression: string;
  command?: string;
  name?: string;
  description?: string;
  last_run?: number;
  next_run?: number;
  lastRun?: string;
  enabled?: boolean;
  status?: string;
}

@customElement("sc-cron-view")
export class ScCronView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 960px;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .job-list {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xl);
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
      font-variant-numeric: tabular-nums;
    }
    .job-actions {
      display: flex;
      gap: var(--sc-space-sm);
      flex-shrink: 0;
    }
    .job-item:nth-child(odd) sc-card {
      --sc-card-bg: var(--sc-bg-surface);
    }
    .job-item:nth-child(even) sc-card {
      --sc-card-bg: var(--sc-bg-inset);
    }
    .form-overlay {
      position: fixed;
      inset: 0;
      background: var(--sc-overlay, rgba(0, 0, 0, 0.6));
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 1000;
    }
    .form-card {
      padding: var(--sc-space-lg);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      width: 100%;
      max-width: 400px;
    }
    .form-title {
      margin: 0 0 var(--sc-space-md);
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
    }
    .form-group {
      margin-bottom: var(--sc-space-md);
    }
    .form-group label {
      display: block;
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-xs);
    }
    .form-group input {
      width: 100%;
      padding: var(--sc-space-sm);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
      box-sizing: border-box;
    }
    .form-actions {
      display: flex;
      gap: var(--sc-space-sm);
      justify-content: flex-end;
      margin-top: var(--sc-space-md);
    }
    @media (max-width: 768px) {
      .header {
        flex-wrap: wrap;
      }
      .job-card-inner {
        flex-wrap: wrap;
      }
    }
    @media (max-width: 480px) {
      .form-card {
        max-width: 100%;
      }
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
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading
        ? html`
            <div class="job-list sc-stagger">
              <sc-skeleton variant="card" height="80px"></sc-skeleton>
              <sc-skeleton variant="card" height="80px"></sc-skeleton>
            </div>
          `
        : this.jobs.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons.timer}
                heading="No scheduled jobs"
                description="Add a cron job to run commands on a schedule."
              ></sc-empty-state>
            `
          : html`
              <div class="job-list sc-stagger">
                ${this.jobs.map(
                  (job) => html`
                    <div class="job-item">
                      <sc-card>
                        <div class="job-card-inner">
                          <div class="job-info">
                            <div class="job-expression">${job.expression}</div>
                            ${job.command
                              ? html`<div class="job-description">
                                  <code>${job.command}</code>
                                </div>`
                              : ""}
                            ${job.name || job.description
                              ? html`<div class="job-description">
                                  ${job.name ?? job.description}
                                </div>`
                              : ""}
                            <div class="job-meta">
                              ${job.last_run && job.last_run > 0
                                ? `Last run: ${new Date(job.last_run * 1000).toLocaleString()}`
                                : job.lastRun
                                  ? `Last run: ${job.lastRun}`
                                  : ""}
                              ${job.enabled === false ? " · disabled" : ""}
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
                    </div>
                  `,
                )}
              </div>
            `}
      ${this.showForm
        ? html`
            <div
              class="form-overlay"
              role="dialog"
              aria-modal="true"
              aria-label="Add cron job"
              @click=${this.closeForm}
            >
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
