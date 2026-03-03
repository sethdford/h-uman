import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

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

/* 20×20 stroked icons, stroke-width 1.5, stroke currentColor, fill none */
const icons = {
  grid: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <rect x="2" y="2" width="6" height="6" />
    <rect x="12" y="2" width="6" height="6" />
    <rect x="2" y="12" width="6" height="6" />
    <rect x="12" y="12" width="6" height="6" />
  </svg>`,
  "message-square": html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M3 4a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H7l-2 2V4z" />
  </svg>`,
  clock: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <circle cx="10" cy="10" r="7" />
    <path d="M10 6v4l2 2" />
  </svg>`,
  cpu: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <rect x="4" y="4" width="12" height="12" rx="1" />
    <path
      d="M7 4V2M13 4V2M7 18v-2M13 18v-2M4 7H2M4 13H2M18 7h2M18 13h2M2 10h2M18 10h2M4 10h12M10 4v12"
    />
  </svg>`,
  box: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M3 6l7-3 7 3M3 6v8l7 3 7-3V6M10 3v14" />
  </svg>`,
  mic: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M10 1a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
    <path d="M15 8v1a5 5 0 0 1-10 0V8" />
    <line x1="10" y1="16" x2="10" y2="19" />
    <line x1="7" y1="19" x2="13" y2="19" />
  </svg>`,
  wrench: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M14.7 6.3a1 1 0 0 0 0-1.4l-1.4-1.4a1 1 0 0 0-1.4 0l-5 5a4 4 0 1 0 1.4 1.4l5-5z" />
    <circle cx="6" cy="14" r="2" />
  </svg>`,
  radio: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M5 15h10a2 2 0 0 0 2-2V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2z" />
    <path d="M10 12a2 2 0 1 0 0-4 2 2 0 0 0 0 4z" />
    <line x1="2" y1="9" x2="18" y2="9" />
  </svg>`,
  puzzle: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path
      d="M4 4h4v4c0 1.1.9 2 2 2h4c1.1 0 2-.9 2-2V4h4v12h-4v-4c0-1.1-.9-2-2-2h-4c-1.1 0-2 .9-2 2v4H4V4z"
    />
  </svg>`,
  timer: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <circle cx="10" cy="10" r="7" />
    <path d="M10 6v4l3 3" />
  </svg>`,
  settings: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <circle cx="10" cy="10" r="2.5" />
    <path
      d="M10 3v1M10 16v1M16 10h1M3 10h1M14.5 5.5l.7.7M4.8 15.2l.7.7M15.2 14.5l.7-.7M4.2 4.8l.7-.7M5.5 14.5l-.7.7M15.2 4.8l-.7-.7M4.8 4.2l-.7-.7"
    />
  </svg>`,
  server: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <rect x="2" y="2" width="16" height="5" rx="1" />
    <rect x="2" y="9" width="16" height="5" rx="1" />
    <line x1="6" y1="5" x2="6" y2="5" />
    <line x1="10" y1="5" x2="10" y2="5" />
    <line x1="6" y1="12" x2="6" y2="12" />
    <line x1="10" y1="12" x2="10" y2="12" />
  </svg>`,
  "bar-chart": html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M4 17V14M10 17V10M16 17V6" />
  </svg>`,
  terminal: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <rect x="2" y="3" width="16" height="14" rx="1" />
    <path d="M6 8l4 4-4 4M11 13h3" />
  </svg>`,
  chevron: html`<svg
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path d="M12 6l-4 4 4 4" />
  </svg>`,
};

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
      { id: "agents", label: "Agents", icon: icons.cpu },
      { id: "models", label: "Models", icon: icons.box },
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
      border-left: 2px solid transparent;
      border-radius: var(--sc-radius-sm);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast),
        color var(--sc-duration-fast);
      margin-bottom: var(--sc-space-xs);
      text-align: left;
      font-family: inherit;
    }

    .nav-item:hover {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }

    .nav-item.active {
      background: var(--sc-accent-subtle);
      color: var(--sc-accent);
      border-left-color: var(--sc-accent);
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

    .nav-item .icon svg {
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
