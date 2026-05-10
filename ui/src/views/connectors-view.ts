import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/hu-card.js";
import "../components/hu-button.js";
import "../components/hu-empty-state.js";
import "../components/hu-skeleton.js";
import "../components/hu-section-header.js";
import "../components/hu-badge.js";

interface McpServer {
  name: string;
  status: string;
  tools_count: number;
  resources_count: number;
}

interface McpTool {
  name: string;
  description: string;
  server: string;
}

interface McpResource {
  uri: string;
  name: string;
  description: string;
  mimeType?: string;
}

@customElement("hu-connectors-view")
export class ConnectorsView extends GatewayAwareLitElement {
  @state() private _servers: McpServer[] = [];
  @state() private _tools: McpTool[] = [];
  @state() private _resources: McpResource[] = [];
  @state() private _loading = false;
  @state() private _error = "";
  @state() private _activeTab: "servers" | "tools" | "resources" = "servers";

  static override styles = css`
    :host {
      display: block;
      container-type: inline-size;
    }

    .tabs {
      display: flex;
      gap: var(--hu-space-xs);
      margin-bottom: var(--hu-space-lg);
      border-bottom: 1px solid var(--hu-border-subtle);
      padding-bottom: var(--hu-space-xs);
    }

    .tab-btn {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: none;
      border: none;
      border-bottom: 2px solid transparent;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .tab-btn:hover {
      color: var(--hu-text);
    }

    .tab-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: -2px;
    }

    .tab-btn[aria-selected="true"] {
      color: var(--hu-accent);
      border-bottom-color: var(--hu-accent);
      font-weight: var(--hu-weight-semibold);
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(16rem, 1fr));
      gap: var(--hu-space-md);
    }

    .server-card {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
    }

    .server-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
    }

    .server-name {
      font-weight: var(--hu-weight-semibold);
      font-size: var(--hu-text-base);
    }

    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      font-size: var(--hu-text-xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-full);
    }

    .status-badge.connected {
      background: color-mix(in srgb, var(--hu-success) 15%, transparent);
      color: var(--hu-success);
    }

    .status-badge.disconnected {
      background: color-mix(in srgb, var(--hu-text-tertiary) 15%, transparent);
      color: var(--hu-text-tertiary);
    }

    .server-stats {
      display: flex;
      gap: var(--hu-space-md);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
    }

    .tool-row,
    .resource-row {
      display: flex;
      align-items: flex-start;
      gap: var(--hu-space-md);
      padding: var(--hu-space-md);
      border-bottom: 1px solid var(--hu-border-subtle);
    }

    .tool-row:last-child,
    .resource-row:last-child {
      border-bottom: none;
    }

    .item-icon {
      flex-shrink: 0;
      width: var(--hu-icon-md, 1.25rem);
      height: var(--hu-icon-md, 1.25rem);
      color: var(--hu-text-secondary);
      margin-top: var(--hu-space-3xs);
    }

    .item-info {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-3xs);
      min-width: 0;
    }

    .item-name {
      font-weight: var(--hu-weight-medium);
      font-size: var(--hu-text-sm);
    }

    .item-desc {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-secondary);
    }

    .item-meta {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-tertiary);
    }

    .list-container {
      background: var(--hu-surface-container);
      border-radius: var(--hu-radius);
      border: 1px solid var(--hu-border-subtle);
      overflow: hidden;
    }

    .section-count {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-tertiary);
      margin-left: var(--hu-space-2xs);
    }

    @container (max-width: 30rem) /* cq-compact */ {
      .tabs {
        overflow-x: auto;
        scrollbar-width: none;
      }
      .grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this._error = "Not connected to gateway";
      return;
    }
    this._loading = true;
    this._error = "";
    try {
      const [serversRes, toolsRes, resourcesRes] = await Promise.all([
        gw.request<{ servers?: McpServer[] }>("mcp.servers.list"),
        gw.request<{ tools?: McpTool[] }>("mcp.tools.list"),
        gw.request<{ resources?: McpResource[] }>("mcp.resources.list"),
      ]);
      this._servers = serversRes?.servers ?? [];
      this._tools = toolsRes?.tools ?? [];
      this._resources = resourcesRes?.resources ?? [];
    } catch (err) {
      this._error = err instanceof Error ? err.message : "Failed to load connectors";
    } finally {
      this._loading = false;
    }
  }

  override render() {
    if (this._error) {
      return html`
        <hu-empty-state
          .icon=${icons.warning}
          heading="Connection Error"
          description=${this._error}
        >
          <hu-button variant="primary" @click=${() => this.load()}>Retry</hu-button>
        </hu-empty-state>
      `;
    }

    if (this._loading) {
      return html`
        <div class="grid">
          <hu-skeleton height="6rem"></hu-skeleton>
          <hu-skeleton height="6rem"></hu-skeleton>
          <hu-skeleton height="6rem"></hu-skeleton>
        </div>
      `;
    }

    return html`
      <div class="tabs" role="tablist">
        <button
          class="tab-btn"
          role="tab"
          aria-selected=${String(this._activeTab === "servers")}
          @click=${() => {
            this._activeTab = "servers";
          }}
        >
          Servers <span class="section-count">(${this._servers.length})</span>
        </button>
        <button
          class="tab-btn"
          role="tab"
          aria-selected=${String(this._activeTab === "tools")}
          @click=${() => {
            this._activeTab = "tools";
          }}
        >
          Tools <span class="section-count">(${this._tools.length})</span>
        </button>
        <button
          class="tab-btn"
          role="tab"
          aria-selected=${String(this._activeTab === "resources")}
          @click=${() => {
            this._activeTab = "resources";
          }}
        >
          Resources <span class="section-count">(${this._resources.length})</span>
        </button>
      </div>

      ${this._activeTab === "servers" ? this._renderServers() : nothing}
      ${this._activeTab === "tools" ? this._renderTools() : nothing}
      ${this._activeTab === "resources" ? this._renderResources() : nothing}
    `;
  }

  private _renderServers() {
    if (this._servers.length === 0) {
      return html`
        <hu-empty-state
          .icon=${icons.cpu}
          heading="No MCP servers"
          description="Connect MCP servers to extend capabilities."
        ></hu-empty-state>
      `;
    }
    return html`
      <div class="grid">
        ${this._servers.map(
          (s) => html`
            <hu-card>
              <div class="server-card">
                <div class="server-header">
                  <span class="server-name">${s.name}</span>
                  <span class="status-badge ${s.status}">
                    ${s.status === "connected" ? icons.check : icons.x} ${s.status}
                  </span>
                </div>
                <div class="server-stats">
                  <span>${s.tools_count} tools</span>
                  <span>${s.resources_count} resources</span>
                </div>
              </div>
            </hu-card>
          `,
        )}
      </div>
    `;
  }

  private _renderTools() {
    if (this._tools.length === 0) {
      return html`
        <hu-empty-state
          .icon=${icons.wrench}
          heading="No MCP tools"
          description="MCP server tools will appear here when connected."
        ></hu-empty-state>
      `;
    }
    return html`
      <div class="list-container">
        ${this._tools.map(
          (t) => html`
            <div class="tool-row">
              <span class="item-icon">${icons.wrench}</span>
              <div class="item-info">
                <span class="item-name">${t.name}</span>
                <span class="item-desc">${t.description}</span>
                <span class="item-meta">Server: ${t.server}</span>
              </div>
            </div>
          `,
        )}
      </div>
    `;
  }

  private _renderResources() {
    if (this._resources.length === 0) {
      return html`
        <hu-empty-state
          .icon=${icons["file-text"]}
          heading="No MCP resources"
          description="MCP server resources will appear here when available."
        ></hu-empty-state>
      `;
    }
    return html`
      <div class="list-container">
        ${this._resources.map(
          (r) => html`
            <div class="resource-row">
              <span class="item-icon">${icons["file-text"]}</span>
              <div class="item-info">
                <span class="item-name">${r.name}</span>
                <span class="item-desc">${r.description}</span>
                <span class="item-meta">${r.uri}${r.mimeType ? ` (${r.mimeType})` : ""}</span>
              </div>
            </div>
          `,
        )}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-connectors-view": ConnectorsView;
  }
}
