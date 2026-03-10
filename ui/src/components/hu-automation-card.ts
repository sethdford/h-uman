import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./hu-card.js";
import "./hu-button.js";
import "./hu-data-table-v2.js";

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

function cronToHuman(expr: string): string {
  const parts = expr.trim().split(/\s+/);
  if (parts.length < 5) return expr;
  const [min, hour, dom, month, dow] = parts;
  const every = (f: string, unit: string) =>
    f === "*" ? `Every ${unit}` : f.startsWith("*/") ? `Every ${f.slice(2)} ${unit}s` : null;
  if (min === "*" && hour === "*" && dom === "*" && month === "*" && dow === "*")
    return "Every minute";
  if (every(min, "minute")) return every(min, "minute")!;
  if (every(hour, "hour") && min === "0") return every(hour, "hour")!;
  if (hour !== "*" && !hour.startsWith("*/") && min === "0") {
    const h = parseInt(hour, 10);
    const ampm = h >= 12 ? (h === 12 ? "noon" : `${h - 12}pm`) : h === 0 ? "midnight" : `${h}am`;
    if (dom === "*" && month === "*" && dow === "*") return `Daily at ${ampm}`;
    if (dow !== "*" && dom === "*" && month === "*") {
      const dayLabel = dow === "1-5" ? "Weekdays" : dow === "0,6" ? "Weekends" : `Days ${dow}`;
      return `${dayLabel} at ${ampm}`;
    }
  }
  if (dom !== "*" && month === "*" && hour === "0" && min === "0") return `Monthly on day ${dom}`;
  return expr;
}

function relativeTime(epoch: number): string {
  const now = Math.floor(Date.now() / 1000);
  const diff = epoch - now;
  const abs = Math.abs(diff);
  const sign = diff >= 0 ? "in " : "";
  const suffix = diff < 0 ? " ago" : "";
  if (abs < 60) return diff >= 0 ? "in <1m" : "<1m ago";
  if (abs < 3600) {
    const m = Math.floor(abs / 60);
    return `${sign}${m}m${suffix}`;
  }
  if (abs < 86400) {
    const h = Math.floor(abs / 3600);
    return `${sign}${h}h${suffix}`;
  }
  if (abs < 604800) {
    const d = Math.floor(abs / 86400);
    return `${sign}${d}d${suffix}`;
  }
  if (abs < 2592000) {
    const w = Math.floor(abs / 604800);
    return `${sign}${w}w${suffix}`;
  }
  return new Date(epoch * 1000).toLocaleDateString(undefined, {
    month: "short",
    day: "numeric",
    year: abs >= 31536000 ? "numeric" : undefined,
  });
}

@customElement("hu-automation-card")
export class ScAutomationCard extends LitElement {
  @property({ type: Object }) job: CronJob | null = null;
  @property({ type: Array }) runs: CronRun[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    .card-wrapper {
      position: relative;
      border-left: 0.1875rem solid transparent;
      border-radius: var(--hu-radius-xl);
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .card-wrapper.paused {
      opacity: 0.7;
      border-left-color: var(--hu-accent-secondary);
    }

    .card-wrapper.last-failed {
      border-left-color: var(--hu-error);
    }

    hu-card {
      display: block;
    }

    .card-content {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .header-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      min-width: 0;
    }

    .toggle-wrap {
      flex-shrink: 0;
    }

    .toggle {
      position: relative;
      display: inline-block;
      width: 2.25rem;
      height: 1.25rem;
      cursor: pointer;
    }

    .toggle input {
      opacity: 0;
      width: 0;
      height: 0;
    }

    .toggle-slider {
      position: absolute;
      inset: 0;
      background: var(--hu-border);
      border-radius: var(--hu-radius-full);
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .toggle-slider::before {
      content: "";
      position: absolute;
      height: 1rem;
      width: 1rem;
      left: 2px; /* hu-lint-ok: sub-token toggle position */
      bottom: 2px; /* hu-lint-ok: sub-token toggle position */
      background: var(--hu-bg);
      border-radius: var(--hu-radius-full);
      box-shadow: var(--hu-shadow-sm);
      transition:
        transform var(--hu-duration-fast) var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .toggle input:checked + .toggle-slider {
      background: var(--hu-accent);
    }

    .toggle input:checked + .toggle-slider::before {
      transform: translateX(1rem);
    }

    .toggle input:focus-visible + .toggle-slider {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .toggle:has(input:disabled) {
      cursor: not-allowed;
      opacity: var(--hu-opacity-disabled);
    }

    .btn-icon {
      display: inline-flex;
      width: 1em;
      height: 1em;
    }

    .btn-icon svg {
      width: 100%;
      height: 100%;
    }

    @media (prefers-reduced-motion: reduce) {
      .toggle-slider,
      .toggle-slider::before,
      .card-wrapper {
        transition: none !important;
      }
    }

    .job-name {
      flex: 1;
      min-width: 0;
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .type-badge {
      flex-shrink: 0;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
    }

    .schedule-block {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .schedule-label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }

    .schedule-expr {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .preview-block {
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .preview-block code {
      font-family: var(--hu-font-mono);
      font-size: inherit;
      background: transparent;
      padding: 0;
    }

    .status-trio {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
      align-items: center;
    }

    .status-item {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .status-item.channel {
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      background: var(--hu-bg-elevated);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text);
    }

    .status-item.next {
      font-variant-numeric: tabular-nums;
    }

    .status-item.last-status.completed {
      color: var(--hu-accent);
    }

    .status-item.last-status.failed {
      color: var(--hu-error);
    }

    .run-history-section {
      margin-top: var(--hu-space-md);
      padding-top: var(--hu-space-md);
      border-top: 1px solid var(--hu-border-subtle);
    }

    .run-history-label {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      margin-bottom: var(--hu-space-sm);
    }

    .run-history {
      display: flex;
      gap: var(--hu-space-xs);
      align-items: center;
    }

    .run-dot {
      width: 8px;
      height: 8px;
      border-radius: var(--hu-radius-full);
      flex-shrink: 0;
    }

    .run-dot.completed {
      background: var(--hu-accent);
    }

    .run-dot.failed {
      background: var(--hu-error);
    }

    .run-dot.empty {
      background: var(--hu-border);
    }

    .footer-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-md);
      flex-wrap: wrap;
    }

    .footer-actions {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .footer-actions .icon-btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      width: 2rem;
      height: 2rem;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .footer-actions .icon-btn:hover {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }

    .footer-actions .icon-btn:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .footer-actions .icon-btn svg {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
    }

    .created-at {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      font-variant-numeric: tabular-nums;
    }

    @media (max-width: 480px) /* --hu-breakpoint-sm */ {
      .footer-row {
        flex-direction: column;
        align-items: flex-start;
      }
    }
  `;

  private _fire(eventName: string): void {
    if (!this.job) return;
    this.dispatchEvent(
      new CustomEvent(eventName, {
        detail: { job: this.job },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onToggle(): void {
    this._fire("automation-toggle");
  }

  private _onRun(): void {
    this._fire("automation-run");
  }

  private _onEdit(): void {
    this._fire("automation-edit");
  }

  private _onDelete(): void {
    this._fire("automation-delete");
  }

  override render() {
    const job = this.job;
    if (!job) return nothing;

    const paused = job.paused;
    const lastFailed = job.last_status === "failed";
    const humanSchedule = cronToHuman(job.expression);
    const nextRunStr = job.next_run > 0 ? relativeTime(job.next_run) : "—";
    const lastStatusClass =
      job.last_status === "completed" ? "completed" : job.last_status === "failed" ? "failed" : "";
    const runRows = this.runs
      .map((r) => ({
        time: new Date(r.started_at * 1000).toLocaleString(undefined, {
          month: "short",
          day: "numeric",
          hour: "2-digit",
          minute: "2-digit",
        }),
        status: r.status,
        duration:
          r.finished_at > r.started_at ? `${Math.round(r.finished_at - r.started_at)}s` : "—",
      }))
      .reverse();
    const createdStr =
      job.created_at > 0
        ? new Date(job.created_at * 1000).toLocaleDateString(undefined, {
            month: "short",
            day: "numeric",
            year: "numeric",
          })
        : "";

    const wrapperClasses = ["card-wrapper", paused ? "paused" : "", lastFailed ? "last-failed" : ""]
      .filter(Boolean)
      .join(" ");

    return html`
      <div class=${wrapperClasses}>
        <hu-card>
          <div class="card-content">
            <div class="header-row">
              <label
                class="toggle-wrap"
                aria-label=${paused ? "Resume automation" : "Pause automation"}
              >
                <span class="toggle">
                  <input
                    type="checkbox"
                    ?checked=${!paused}
                    @change=${this._onToggle}
                    aria-checked=${!paused}
                  />
                  <span class="toggle-slider"></span>
                </span>
              </label>
              <span class="job-name" title=${job.name}>${job.name || "Unnamed"}</span>
              <span class="type-badge">${job.type === "agent" ? "Agent" : "Shell"}</span>
            </div>

            <div class="schedule-block">
              <span class="schedule-label">${humanSchedule}</span>
              <span class="schedule-expr">${job.expression}</span>
            </div>

            <div class="preview-block">
              ${job.type === "agent"
                ? html`<span>${job.command || "—"}</span>`
                : html`<code>${job.command || "—"}</code>`}
            </div>

            <div class="status-trio">
              <span class="status-item channel">${job.channel || "—"}</span>
              <span class="status-item next">Next: ${nextRunStr}</span>
              <span class="status-item last-status ${lastStatusClass}">
                ${job.last_status === "completed"
                  ? "completed"
                  : job.last_status === "failed"
                    ? "failed"
                    : "—"}
              </span>
            </div>

            ${this.runs.length > 0
              ? html`
                  <div class="run-history-section">
                    <div class="run-history-label">Run history</div>
                    <hu-data-table-v2
                      .columns=${[
                        { key: "time", label: "Time", sortable: true },
                        { key: "status", label: "Status", sortable: true },
                        { key: "duration", label: "Duration", align: "right" as const },
                      ]}
                      .rows=${runRows}
                      .pageSize=${5}
                      paginated
                    ></hu-data-table-v2>
                  </div>
                `
              : nothing}

            <div class="footer-row">
              <div class="footer-actions">
                <hu-button variant="secondary" size="sm" @click=${this._onRun}>
                  <span class="btn-icon" aria-hidden="true">${icons.zap}</span>
                  Run Now
                </hu-button>
                <hu-button variant="ghost" size="sm" @click=${this._onEdit}>
                  <span class="btn-icon" aria-hidden="true">${icons["pencil-simple"]}</span>
                  Edit
                </hu-button>
                <button
                  class="icon-btn"
                  type="button"
                  aria-label="Delete automation"
                  @click=${this._onDelete}
                >
                  ${icons.trash}
                </button>
              </div>
              ${createdStr ? html`<span class="created-at">Created ${createdStr}</span>` : nothing}
            </div>
          </div>
        </hu-card>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-automation-card": ScAutomationCard;
  }
}
