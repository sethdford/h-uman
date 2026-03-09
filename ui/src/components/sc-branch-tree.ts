import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export interface BranchNode {
  id: string;
  label: string;
  children: BranchNode[];
  messagePreview: string;
  active: boolean;
}

@customElement("sc-branch-tree")
export class ScBranchTree extends LitElement {
  @property({ type: Array }) branches: BranchNode[] = [];
  @property() activeId = "";

  @state() private _collapsed = new Set<string>();

  static override styles = css`
    :host {
      display: block;
    }
    .branch-tree {
      display: flex;
      flex-direction: column;
      gap: 0;
    }
    .branch-node {
      display: flex;
      flex-direction: row;
      align-items: flex-start;
      gap: var(--sc-space-sm);
      cursor: pointer;
      padding: var(--sc-space-xs) 0;
      position: relative;
    }
    .branch-node::before {
      content: "";
      position: absolute;
      left: calc(var(--sc-space-sm) / 2 - 1px);
      top: calc(var(--sc-space-md) + var(--sc-space-xs));
      bottom: calc(-1 * var(--sc-space-xs));
      width: 2px;
      background: var(--sc-border-subtle);
    }
    .branch-node:last-child::before {
      display: none;
    }
    .branch-node .node-dot {
      flex-shrink: 0;
      width: var(--sc-space-sm);
      height: var(--sc-space-sm);
      border-radius: var(--sc-radius-full);
      background: var(--sc-border-subtle);
      margin-top: var(--sc-space-2xs);
      position: relative;
      z-index: 1;
    }
    .branch-node.active .node-dot {
      background: var(--sc-accent);
      box-shadow: 0 0 0 2px color-mix(in srgb, var(--sc-accent) 30%, transparent);
    }
    .branch-content {
      flex: 1;
      min-width: 0;
    }
    .branch-node .node-label {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .branch-node.active .node-label {
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .branch-node .node-preview {
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      max-width: 100%;
      margin-top: var(--sc-space-2xs);
    }
    .branch-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      width: 100%;
    }
    .branch-children {
      margin-left: var(--sc-space-md);
      border-left: 2px solid var(--sc-border-subtle);
      padding-left: var(--sc-space-sm);
      margin-top: var(--sc-space-2xs);
    }
    .branch-line {
      width: 2px;
      flex-shrink: 0;
      background: var(--sc-border-subtle);
      align-self: stretch;
      min-height: var(--sc-space-md);
    }
    .branch-toggle {
      flex-shrink: 0;
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      display: flex;
      align-items: center;
      justify-content: center;
      background: transparent;
      border: none;
      cursor: pointer;
      color: var(--sc-text-muted);
      padding: 0;
      border-radius: var(--sc-radius-sm);
    }
    .branch-toggle:hover {
      color: var(--sc-accent);
      background: var(--sc-hover-overlay);
    }
    .branch-toggle svg {
      width: 0.75rem;
      height: 0.75rem;
      transition: transform var(--sc-duration-fast) var(--sc-ease-out);
    }
    .branch-node.collapsed .branch-toggle svg {
      transform: rotate(-90deg);
    }
    @media (prefers-reduced-motion: reduce) {
      .branch-toggle svg {
        transition: none;
      }
    }
  `;

  private _onNodeClick(e: Event, node: BranchNode): void {
    e.stopPropagation();
    this.dispatchEvent(
      new CustomEvent("sc-branch-select", {
        bubbles: true,
        composed: true,
        detail: { id: node.id },
      }),
    );
  }

  private _toggleCollapse(nodeId: string): void {
    this._collapsed = new Set(this._collapsed);
    if (this._collapsed.has(nodeId)) {
      this._collapsed.delete(nodeId);
    } else {
      this._collapsed.add(nodeId);
    }
    this.requestUpdate();
  }

  private _renderNode(node: BranchNode, _depth = 0): ReturnType<typeof html> {
    const isActive = node.active || node.id === this.activeId;
    const hasChildren = node.children?.length > 0;
    const isCollapsed = hasChildren && this._collapsed.has(node.id);

    return html`
      <div class="branch-node ${isActive ? "active" : ""} ${isCollapsed ? "collapsed" : ""}">
        <div class="branch-row">
          <span class="node-dot" aria-hidden="true"></span>
          <div class="branch-content">
            <div
              role="button"
              tabindex="0"
              class="node-label"
              @click=${(e: Event) => this._onNodeClick(e, node)}
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  this._onNodeClick(e, node);
                }
              }}
            >
              ${node.label}
            </div>
            <div class="node-preview" title=${node.messagePreview}>${node.messagePreview}</div>
          </div>
          ${hasChildren
            ? html`
                <button
                  type="button"
                  class="branch-toggle"
                  aria-label=${isCollapsed ? "Expand subtree" : "Collapse subtree"}
                  @click=${(e: Event) => {
                    e.stopPropagation();
                    this._toggleCollapse(node.id);
                  }}
                >
                  <svg viewBox="0 0 256 256" fill="currentColor">
                    <path
                      d="M213.66,101.66l-80,80a8,8,0,0,1-11.32,0l-80-80a8,8,0,0,1,11.32-11.32L128,164.69l74.34-74.35a8,8,0,0,1,11.32,11.32Z"
                    />
                  </svg>
                </button>
              `
            : nothing}
        </div>
        ${hasChildren && !isCollapsed
          ? html`
              <div class="branch-children">
                ${node.children.map((child) => this._renderNode(child, _depth + 1))}
              </div>
            `
          : nothing}
      </div>
    `;
  }

  override render() {
    if (!this.branches?.length) return html``;
    return html`
      <div class="branch-tree" role="tree" aria-label="Conversation branches">
        ${this.branches.map((node) => this._renderNode(node))}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-branch-tree": ScBranchTree;
  }
}
