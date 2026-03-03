import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

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
      margin-bottom: 1rem;
    }
    h2 {
      margin: 0;
      font-size: 1.25rem;
      font-weight: 600;
      color: var(--sc-text);
    }
    .btn {
      padding: 0.5rem 1rem;
      background: var(--sc-accent);
      color: white;
      border: none;
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: 0.875rem;
    }
    .btn:hover {
      background: var(--sc-accent-hover);
    }
    .btn-secondary {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }
    .btn-secondary:hover {
      background: var(--sc-border);
    }
    .btn-danger {
      background: var(--sc-error-dim);
      color: white;
    }
    .btn-danger:hover {
      background: var(--sc-error);
    }
    .job-list {
      display: flex;
      flex-direction: column;
      gap: 0.75rem;
    }
    .job-card {
      padding: 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: 1rem;
    }
    .job-info {
      flex: 1;
      min-width: 0;
    }
    .job-expression {
      font-family: var(--sc-font-mono);
      font-size: 0.875rem;
      color: var(--sc-accent);
      margin-bottom: 0.25rem;
    }
    .job-description {
      font-size: 0.875rem;
      color: var(--sc-text-muted);
      margin-bottom: 0.25rem;
    }
    .job-meta {
      font-size: 0.75rem;
      color: var(--sc-text-muted);
    }
    .job-actions {
      display: flex;
      gap: 0.5rem;
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
    .error {
      color: var(--sc-accent);
      font-size: 0.875rem;
      margin-top: 0.5rem;
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
        <button class="btn" @click=${this.openAddForm}>Add Job</button>
      </div>
      ${this.error ? html`<p class="error">${this.error}</p>` : ""}
      ${this.loading
        ? html`
            <div class="job-list">
              <div class="job-card skeleton skeleton-card"></div>
              <div class="job-card skeleton skeleton-card"></div>
            </div>
          `
        : this.jobs.length === 0
          ? html`
              <div class="empty-state">
                <div class="empty-icon">⏰</div>
                <p class="empty-title">No scheduled jobs</p>
                <p class="empty-desc">Add a cron job to run commands on a schedule.</p>
              </div>
            `
          : html`
              <div class="job-list">
                ${this.jobs.map(
                  (job) => html`
                    <div class="job-card">
                      <div class="job-info">
                        <div class="job-expression">${job.expression}</div>
                        ${job.command
                          ? html`<div class="job-description">
                              <code>${job.command}</code>
                            </div>`
                          : ""}
                        ${job.description
                          ? html`<div class="job-description">${job.description}</div>`
                          : ""}
                        <div class="job-meta">
                          ${job.lastRun ? `Last run: ${job.lastRun}` : ""}
                          ${job.status ? ` · ${job.status}` : ""}
                        </div>
                      </div>
                      <div class="job-actions">
                        <button
                          class="btn btn-secondary"
                          ?disabled=${job.id == null}
                          @click=${() => this.runJob(job)}
                        >
                          Run Now
                        </button>
                        <button
                          class="btn btn-danger"
                          ?disabled=${job.id == null}
                          @click=${() => this.deleteJob(job)}
                        >
                          Delete
                        </button>
                      </div>
                    </div>
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
                  <button class="btn btn-secondary" @click=${this.closeForm}>Cancel</button>
                  <button class="btn" @click=${this.submitAdd}>Add</button>
                </div>
              </div>
            </div>
          `
        : ""}
    `;
  }
}
