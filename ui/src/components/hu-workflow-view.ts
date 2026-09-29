import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-approval-gate.js";
import "./hu-workflow-timeline.js";
import "./hu-agent-graph.js";
import type { WorkflowEvent } from "./hu-workflow-timeline.js";
import type { Agent } from "./hu-agent-graph.js";

export interface WorkflowViewConfig {
  workflowId: string;
  status: "running" | "paused" | "completed" | "failed";
  events: WorkflowEvent[];
  agents: Agent[];
  approvalGates: Array<{
    gateId: string;
    description: string;
    status: "pending" | "approved" | "rejected" | "timed_out";
  }>;
}

@customElement("hu-workflow-detail")
export class HuWorkflowDetail extends LitElement {
  @property() workflowId = "";
  @property() status: "running" | "paused" | "completed" | "failed" = "running";
  @property({ type: Array }) events: WorkflowEvent[] = [];
  @property({ type: Array }) agents: Agent[] = [];
  @property({ type: Array }) approvalGates: Array<{
    gateId: string;
    description: string;
    status: "pending" | "approved" | "rejected" | "timed_out";
  }> = [];

  static override styles = css`
    :host {
      display: block;
      width: 100%;
      font-family: var(--hu-font);
      container-type: inline-size;
      background: var(--hu-bg);
    }

    .workflow-view {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--hu-space-lg);
      padding: var(--hu-space-lg);
      max-width: 1400px;
      margin: 0 auto;
    }

    @container (max-width: 48rem) /* cq-medium */ {
      .workflow-view {
        grid-template-columns: 1fr;
      }
    }

    .panel {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
      padding: var(--hu-space-lg);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-lg);
      animation: hu-workflow-panel-slide-in var(--hu-duration-moderate) var(--hu-ease-out) both;
    }

    .panel:nth-child(1) {
      animation-delay: 0ms;
    }

    .panel:nth-child(2) {
      animation-delay: 50ms;
    }

    .panel:nth-child(3) {
      animation-delay: 100ms;
    }

    @keyframes hu-workflow-panel-slide-in {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-md));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .panel-title {
      font-size: var(--hu-text-lg);
      font-weight: 700;
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-md) 0;
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .panel-icon {
      width: 20px;
      height: 20px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      flex-shrink: 0;
      opacity: 0.7;
    }

    .timeline-panel {
      grid-row: 1;
      grid-column: 1;
    }

    .approval-panel {
      grid-row: 2;
      grid-column: 1;
    }

    .agents-panel {
      grid-row: 1 / 3;
      grid-column: 2;
    }

    @container (max-width: 48rem) /* cq-medium */ {
      .timeline-panel {
        grid-row: 1;
        grid-column: 1;
      }

      .approval-panel {
        grid-row: 2;
        grid-column: 1;
      }

      .agents-panel {
        grid-row: 3;
        grid-column: 1;
      }
    }

    .panel-content {
      flex: 1;
      overflow-y: auto;
      max-height: 600px;
    }

    .panel-content::-webkit-scrollbar {
      width: 6px;
    }

    .panel-content::-webkit-scrollbar-track {
      background: transparent;
    }

    .panel-content::-webkit-scrollbar-thumb {
      background: var(--hu-border);
      border-radius: var(--hu-radius-sm);
    }

    .panel-content::-webkit-scrollbar-thumb:hover {
      background: color-mix(in srgb, var(--hu-border) 120%, transparent);
    }

    .approval-gates {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .empty-message {
      padding: var(--hu-space-lg);
      text-align: center;
      color: var(--hu-text-muted);
      font-size: var(--hu-text-sm);
    }

    hu-approval-gate,
    hu-workflow-timeline,
    hu-agent-graph {
      width: 100%;
    }

    .workflow-header {
      padding: var(--hu-space-lg);
      background: color-mix(in srgb, var(--hu-bg-elevated) 50%, var(--hu-bg-surface));
      border-bottom: 1px solid var(--hu-border);
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: var(--hu-space-md);
    }

    .workflow-title {
      font-size: var(--hu-text-xl);
      font-weight: 700;
      color: var(--hu-text);
      margin: 0;
    }

    .workflow-status {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: var(--hu-bg-surface);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }

    .workflow-status.running {
      background: color-mix(in srgb, var(--hu-accent) 12%, var(--hu-bg-surface));
      color: var(--hu-accent);
      border: 1px solid color-mix(in srgb, var(--hu-accent) 25%, var(--hu-border));
    }

    .workflow-status.paused {
      background: color-mix(in srgb, var(--hu-warning) 12%, var(--hu-bg-surface));
      color: var(--hu-warning);
      border: 1px solid color-mix(in srgb, var(--hu-warning) 25%, var(--hu-border));
    }

    .workflow-status.completed {
      background: color-mix(in srgb, var(--hu-success) 12%, var(--hu-bg-surface));
      color: var(--hu-success);
      border: 1px solid color-mix(in srgb, var(--hu-success) 25%, var(--hu-border));
    }

    .workflow-status.failed {
      background: color-mix(in srgb, var(--hu-error) 12%, var(--hu-bg-surface));
      color: var(--hu-error);
      border: 1px solid color-mix(in srgb, var(--hu-error) 25%, var(--hu-border));
    }

    .status-dot {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      flex-shrink: 0;
    }

    .status-dot.running {
      background: var(--hu-accent);
      animation: hu-workflow-pulse var(--hu-duration-slowest) var(--hu-ease-in-out) infinite;
    }

    .status-dot.paused {
      background: var(--hu-warning);
    }

    .status-dot.completed {
      background: var(--hu-success);
    }

    .status-dot.failed {
      background: var(--hu-error);
    }

    @keyframes hu-workflow-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.6;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .panel {
        animation: none;
      }
      .status-dot.running {
        animation: none;
      }
    }
  `;

  override render() {
    return html`
      <div class="workflow-header">
        <h1 class="workflow-title">Workflow: ${this.workflowId || "Unknown"}</h1>
        <div class="workflow-status ${this.status}">
          <div class="status-dot ${this.status}"></div>
          ${this._formatStatus(this.status)}
        </div>
      </div>

      <div class="workflow-view">
        <!-- Timeline Panel -->
        <div class="panel timeline-panel">
          <h2 class="panel-title">
            <span class="panel-icon">
              <svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor">
                <path
                  d="M19 3h-1V1h-2v2H8V1H6v2H5c-1.1 0-1.99.9-1.99 2L3 19c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm0 16H5V8h14v11z"
                />
              </svg>
            </span>
            Timeline
          </h2>
          <div class="panel-content">
            ${
              this.events.length > 0
                ? html`
                    <hu-workflow-timeline
                      .events=${this.events}
                      .workflowId=${this.workflowId}
                      .status=${this.status}
                    ></hu-workflow-timeline>
                  `
                : html`<div class="empty-message">No events yet</div>`
            }
          </div>
        </div>

        <!-- Approval Gates Panel -->
        <div class="panel approval-panel">
          <h2 class="panel-title">
            <span class="panel-icon">✓</span>
            Approvals
          </h2>
          <div class="panel-content">
            ${
              this.approvalGates.length > 0
                ? html`
                    <div class="approval-gates">
                      ${this.approvalGates.map(
                        (gate) => html`
                          <hu-approval-gate
                            .gateId=${gate.gateId}
                            .description=${gate.description}
                            .status=${gate.status}
                            @gate-approve=${this._handleGateApprove.bind(this)}
                            @gate-reject=${this._handleGateReject.bind(this)}
                          ></hu-approval-gate>
                        `,
                      )}
                    </div>
                  `
                : html`<div class="empty-message">No approval gates required</div>`
            }
          </div>
        </div>

        <!-- Agents Panel -->
        <div class="panel agents-panel">
          <h2 class="panel-title">
            <span class="panel-icon">🤖</span>
            Agents
          </h2>
          <div class="panel-content">
            ${
              this.agents.length > 0
                ? html` <hu-agent-graph .agents=${this.agents}></hu-agent-graph> `
                : html`<div class="empty-message">No agents assigned</div>`
            }
          </div>
        </div>
      </div>
    `;
  }

  private _handleGateApprove(e: Event) {
    const event = e as CustomEvent;
    this.dispatchEvent(
      new CustomEvent("workflow-gate-approved", {
        detail: { gateId: event.detail.gateId },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _handleGateReject(e: Event) {
    const event = e as CustomEvent;
    this.dispatchEvent(
      new CustomEvent("workflow-gate-rejected", {
        detail: { gateId: event.detail.gateId },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _formatStatus(status: string): string {
    const statusMap: Record<string, string> = {
      running: "Running",
      paused: "Paused",
      completed: "Completed",
      failed: "Failed",
    };
    return statusMap[status] || status;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-workflow-detail": HuWorkflowDetail;
  }
}
