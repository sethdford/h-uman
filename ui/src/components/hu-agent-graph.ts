import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export type AgentRole = "orchestrator" | "worker" | "supervisor" | "monitor";
export type PermissionLevel = "full" | "limited" | "read-only";

export interface Agent {
  id: string;
  name: string;
  role: AgentRole;
  permissionLevel: PermissionLevel;
  status: "active" | "idle" | "busy" | "offline";
  parentId?: string;
  children?: Agent[];
}

@customElement("hu-agent-graph")
export class HuAgentGraph extends LitElement {
  @property({ type: Array }) agents: Agent[] = [];

  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font);
      overflow: auto;
    }

    .graph-container {
      padding: var(--hu-space-lg);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-lg);
    }

    .tree-node {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
      animation: hu-agent-fade-in var(--hu-duration-moderate) var(--hu-ease-out) both;
    }

    @keyframes hu-agent-fade-in {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-sm));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .tree-node.depth-1 {
      margin-left: var(--hu-space-lg);
      animation-delay: 50ms;
    }

    .tree-node.depth-2 {
      margin-left: calc(var(--hu-space-lg) * 2);
      animation-delay: 100ms;
    }

    .tree-node.depth-3 {
      margin-left: calc(var(--hu-space-lg) * 3);
      animation-delay: 150ms;
    }

    .agent-node {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
      padding: var(--hu-space-md);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-lg);
      transition: all var(--hu-duration-fast) var(--hu-ease-out);
      position: relative;
    }

    .agent-node:hover {
      border-color: var(--hu-accent);
      background: color-mix(in srgb, var(--hu-accent) 3%, var(--hu-bg-surface));
    }

    .agent-node.status-active {
      border-color: color-mix(in srgb, var(--hu-success) 40%, var(--hu-border));
    }

    .agent-node.status-busy {
      border-color: color-mix(in srgb, var(--hu-accent) 40%, var(--hu-border));
    }

    .agent-node.status-idle {
      border-color: color-mix(in srgb, var(--hu-warning) 40%, var(--hu-border));
    }

    .agent-node.status-offline {
      border-color: color-mix(in srgb, var(--hu-error) 40%, var(--hu-border));
      opacity: 0.7;
    }

    .status-indicator {
      width: 12px;
      height: 12px;
      border-radius: 50%;
      flex-shrink: 0;
      animation: hu-agent-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .status-indicator.active {
      background: var(--hu-success);
    }

    .status-indicator.busy {
      background: var(--hu-accent);
    }

    .status-indicator.idle {
      background: var(--hu-warning);
    }

    .status-indicator.offline {
      background: var(--hu-text-muted);
      animation: none;
    }

    @keyframes hu-agent-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.6;
      }
    }

    .agent-info {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
      flex: 1;
      min-width: 0;
    }

    .agent-name {
      font-size: var(--hu-text-sm);
      font-weight: 600;
      color: var(--hu-text);
      margin: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .agent-badges {
      display: flex;
      gap: var(--hu-space-2xs);
      flex-wrap: wrap;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      border-radius: var(--hu-radius-sm);
      font-size: var(--hu-text-2xs);
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.3px;
      background: color-mix(in srgb, var(--hu-bg-elevated) 60%, var(--hu-bg-surface));
      color: var(--hu-text-muted);
      border: 1px solid var(--hu-border);
      white-space: nowrap;
    }

    .badge.role-orchestrator {
      background: color-mix(in srgb, var(--hu-accent) 12%, var(--hu-bg-surface));
      color: var(--hu-accent);
      border-color: color-mix(in srgb, var(--hu-accent) 25%, var(--hu-border));
    }

    .badge.role-worker {
      background: color-mix(in srgb, var(--hu-success) 12%, var(--hu-bg-surface));
      color: var(--hu-success);
      border-color: color-mix(in srgb, var(--hu-success) 25%, var(--hu-border));
    }

    .badge.role-supervisor {
      background: color-mix(in srgb, var(--hu-warning) 12%, var(--hu-bg-surface));
      color: var(--hu-warning);
      border-color: color-mix(in srgb, var(--hu-warning) 25%, var(--hu-border));
    }

    .badge.role-monitor {
      background: color-mix(in srgb, var(--hu-accent-secondary) 12%, var(--hu-bg-surface));
      color: var(--hu-accent-secondary);
      border-color: color-mix(in srgb, var(--hu-accent-secondary) 25%, var(--hu-border));
    }

    .badge.permission-full {
      background: color-mix(in srgb, var(--hu-success) 12%, var(--hu-bg-surface));
      color: var(--hu-success);
      border-color: color-mix(in srgb, var(--hu-success) 25%, var(--hu-border));
    }

    .badge.permission-limited {
      background: color-mix(in srgb, var(--hu-warning) 12%, var(--hu-bg-surface));
      color: var(--hu-warning);
      border-color: color-mix(in srgb, var(--hu-warning) 25%, var(--hu-border));
    }

    .badge.permission-read-only {
      background: color-mix(in srgb, var(--hu-text-muted) 12%, var(--hu-bg-surface));
      color: var(--hu-text-muted);
      border-color: color-mix(in srgb, var(--hu-text-muted) 25%, var(--hu-border));
    }

    .children {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .empty-state {
      padding: var(--hu-space-lg);
      text-align: center;
      color: var(--hu-text-muted);
    }

    .empty-state-text {
      font-size: var(--hu-text-sm);
      margin: 0;
    }

    @media (prefers-reduced-motion: reduce) {
      .tree-node {
        animation: none;
      }
      .agent-node {
        transition: none;
      }
      .status-indicator {
        animation: none;
      }
    }
  `;

  override render() {
    if (!this.agents || this.agents.length === 0) {
      return html`
        <div class="empty-state">
          <p class="empty-state-text">No agents in the graph</p>
        </div>
      `;
    }

    return html`
      <div class="graph-container" role="tree">
        ${this.agents.map((agent) => this._renderNode(agent, 0))}
      </div>
    `;
  }

  private _renderNode(agent: Agent, depth: number): ReturnType<typeof html> {
    const children = agent.children && agent.children.length > 0;

    return html`
      <div class="tree-node depth-${Math.min(depth, 3)}" role="treeitem">
        <div class="agent-node status-${agent.status}">
          <div class="status-indicator ${agent.status}"></div>

          <div class="agent-info">
            <p class="agent-name" title=${agent.name}>${agent.name}</p>
            <div class="agent-badges">
              <span class="badge role-${agent.role}"> ${this._formatRole(agent.role)} </span>
              <span class="badge permission-${agent.permissionLevel}">
                ${this._formatPermission(agent.permissionLevel)}
              </span>
            </div>
          </div>
        </div>

        ${
          children
            ? html`
                <div class="children" role="group">
                  ${agent.children!.map((child) => this._renderNode(child, depth + 1))}
                </div>
              `
            : ""
        }
      </div>
    `;
  }

  private _formatRole(role: AgentRole): string {
    const roleMap: Record<AgentRole, string> = {
      orchestrator: "Orchestrator",
      worker: "Worker",
      supervisor: "Supervisor",
      monitor: "Monitor",
    };
    return roleMap[role] || role;
  }

  private _formatPermission(level: PermissionLevel): string {
    const permMap: Record<PermissionLevel, string> = {
      full: "Full Access",
      limited: "Limited",
      "read-only": "Read-Only",
    };
    return permMap[level] || level;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-agent-graph": HuAgentGraph;
  }
}
