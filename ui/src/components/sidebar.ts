import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

type ConnectionStatus = "connected" | "connecting" | "disconnected";

interface NavItem {
  id: string;
  label: string;
  icon: ReturnType<typeof html>;
}

interface NavSection {
  title: string;
  items: NavItem[];
}

const SECTIONS: NavSection[] = [
  {
    title: "Core",
    items: [
      { id: "overview", label: "Overview", icon: icons.grid },
      { id: "chat", label: "Chat", icon: icons["message-square"] },
      { id: "sessions", label: "Sessions", icon: icons.clock },
    ],
  },
  {
    title: "AI",
    items: [
      { id: "agents", label: "Agents", icon: icons.zap },
      { id: "models", label: "Models", icon: icons.cpu },
      { id: "voice", label: "Voice", icon: icons.mic },
    ],
  },
  {
    title: "Platform",
    items: [
      { id: "tools", label: "Tools", icon: icons.wrench },
      { id: "channels", label: "Channels", icon: icons.radio },
      { id: "skills", label: "Skills", icon: icons.puzzle },
      { id: "cron", label: "Cron", icon: icons.timer },
    ],
  },
  {
    title: "System",
    items: [
      { id: "config", label: "Config", icon: icons.settings },
      { id: "nodes", label: "Nodes", icon: icons.server },
      { id: "usage", label: "Usage", icon: icons["bar-chart"] },
      { id: "logs", label: "Logs", icon: icons.terminal },
    ],
  },
];

@customElement("sc-sidebar")
export class ScSidebar extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      width: var(--sc-sidebar-width);
      min-width: var(--sc-sidebar-width);
      background: var(--sc-bg-surface);
      border-right: 1px solid var(--sc-border-subtle);
      transition: width var(--sc-duration-normal) var(--sc-ease-out);
      overflow: hidden;
    }

    :host([collapsed]) {
      width: var(--sc-sidebar-collapsed);
      min-width: var(--sc-sidebar-collapsed);
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-md);
      flex-shrink: 0;
    }

    .logo {
      flex-shrink: 0;
      width: 24px;
      height: 24px;
      color: var(--sc-accent);
    }

    .logo svg {
      width: 100%;
      height: 100%;
    }

    .brand {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      white-space: nowrap;
      overflow: hidden;
      opacity: 1;
      transition: opacity var(--sc-duration-normal) var(--sc-ease-out);
    }

    :host([collapsed]) .brand {
      opacity: 0;
      width: 0;
      overflow: hidden;
      pointer-events: none;
    }

    .nav {
      flex: 1;
      overflow-y: auto;
      overflow-x: hidden;
      padding: var(--sc-space-sm);
    }

    .section {
      margin-bottom: var(--sc-space-md);
    }

    .section-title {
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      letter-spacing: 0.05em;
      text-transform: uppercase;
      color: var(--sc-text-faint);
      padding: var(--sc-space-xs) var(--sc-space-md);
      margin-bottom: var(--sc-space-xs);
      white-space: nowrap;
      overflow: hidden;
      transition: opacity var(--sc-duration-normal) var(--sc-ease-out);
    }

    :host([collapsed]) .section-title {
      opacity: 0;
      height: 0;
      padding: 0;
      margin: 0;
      overflow: hidden;
    }

    .nav-item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      width: 100%;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: transparent;
      border: none;
      border-left: 3px solid transparent;
      border-radius: var(--sc-radius-sm);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast),
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast);
      margin-bottom: var(--sc-space-xs);
      text-align: left;
      font-family: inherit;
    }

    .nav-item:hover:not(.active) {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }

    .nav-item.active {
      background: var(--sc-accent-subtle);
      border-left: 3px solid var(--sc-accent);
      color: var(--sc-accent);
    }

    :host([collapsed]) .nav-item {
      justify-content: center;
      padding: var(--sc-space-sm);
    }

    :host([collapsed]) .nav-item .label {
      display: none;
    }

    .nav-item .icon {
      flex-shrink: 0;
      width: 20px;
      height: 20px;
    }

    .nav-item .icon svg,
    .nav-item svg {
      flex-shrink: 0;
      width: 100%;
      height: 100%;
    }

    .footer {
      flex-shrink: 0;
      padding: var(--sc-space-md);
      border-top: 1px solid var(--sc-border-subtle);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }

    .status-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    :host([collapsed]) .status-row .status-label {
      display: none;
    }

    :host([collapsed]) .status-row {
      justify-content: center;
    }

    .status-dot {
      flex-shrink: 0;
      width: 8px;
      height: 8px;
      border-radius: 50%;
    }

    .status-dot.connected {
      background: var(--sc-success);
    }

    .status-dot.connecting {
      background: var(--sc-warning);
      animation: sc-pulse 1s ease-in-out infinite;
    }

    .status-dot.disconnected {
      background: var(--sc-text-muted);
    }

    .collapse-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 100%;
      padding: var(--sc-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast),
        color var(--sc-duration-fast),
        transform var(--sc-duration-normal) var(--sc-ease-out);
    }

    .collapse-btn:hover {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }

    :host([collapsed]) .collapse-btn .icon {
      transform: rotate(180deg);
    }

    .collapse-btn .icon {
      width: 20px;
      height: 20px;
    }

    .collapse-btn .icon svg {
      width: 100%;
      height: 100%;
    }

    :host([collapsed]) .collapse-btn .label {
      display: none;
    }

    .collapse-btn .label {
      margin-left: var(--sc-space-sm);
    }
  `;

  @property({ type: String }) activeTab = "overview";
  @property({ type: Boolean, reflect: true }) collapsed = false;
  @property({ type: String }) connectionStatus: ConnectionStatus = "disconnected";

  override render() {
    return html`
      <aside class="sidebar">
        <header class="header">
          <div class="logo" aria-hidden="true">
            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" fill="none">
              <path
                d="M8 24C8 24 6 20 6 16C6 12 8 8 12 6"
                stroke="currentColor"
                stroke-width="2.5"
                stroke-linecap="round"
              />
              <path
                d="M14 24C14 24 12 21 12 18C12 15 13 12 16 10"
                stroke="currentColor"
                stroke-width="2.5"
                stroke-linecap="round"
              />
              <path
                d="M20 24C20 24 18 21 18 18C18 15 19 12 22 10"
                stroke="currentColor"
                stroke-width="2.5"
                stroke-linecap="round"
              />
              <path
                d="M24 24C24 24 26 20 26 16C26 12 24 8 20 6"
                stroke="currentColor"
                stroke-width="2.5"
                stroke-linecap="round"
              />
              <circle cx="16" cy="27" r="2" fill="currentColor" />
            </svg>
          </div>
          <span class="brand">SeaClaw</span>
        </header>

        <nav class="nav">
          ${SECTIONS.map(
            (section) => html`
              <div class="section">
                <div class="section-title">${section.title}</div>
                ${section.items.map(
                  (item) => html`
                    <button
                      class="nav-item ${this.activeTab === item.id ? "active" : ""}"
                      ?aria-current=${this.activeTab === item.id}
                      aria-label=${item.label}
                      title=${this.collapsed ? item.label : undefined}
                      @click=${() => this._dispatchTabChange(item.id)}
                    >
                      <span class="icon">${item.icon}</span>
                      <span class="label">${item.label}</span>
                    </button>
                  `,
                )}
              </div>
            `,
          )}
        </nav>

        <footer class="footer">
          <div class="status-row">
            <span class="status-dot ${this.connectionStatus}" aria-hidden="true"></span>
            <span class="status-label">${this.connectionStatus}</span>
          </div>
          <button
            class="collapse-btn"
            aria-label=${this.collapsed ? "Expand sidebar" : "Collapse sidebar"}
            @click=${this._dispatchToggleCollapse}
          >
            <span class="icon">${icons.chevron}</span>
            <span class="label">${this.collapsed ? "Expand" : "Collapse"}</span>
          </button>
        </footer>
      </aside>
    `;
  }

  private _dispatchTabChange(tabId: string): void {
    this.dispatchEvent(
      new CustomEvent("tab-change", {
        detail: tabId,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _dispatchToggleCollapse = (): void => {
    this.dispatchEvent(
      new CustomEvent("toggle-collapse", {
        bubbles: true,
        composed: true,
      }),
    );
  };
}
