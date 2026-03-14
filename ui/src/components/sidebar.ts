import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./hu-status-dot.js";

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
    ],
  },
  {
    title: "AI",
    items: [
      { id: "agents", label: "Agents", icon: icons.zap },
      { id: "models", label: "Models", icon: icons.cpu },
      { id: "memory", label: "Memory", icon: icons.brain },
      { id: "voice", label: "Voice", icon: icons.mic },
    ],
  },
  {
    title: "Platform",
    items: [
      { id: "tools", label: "Tools", icon: icons.wrench },
      { id: "channels", label: "Channels", icon: icons.radio },
      { id: "skills", label: "Skills", icon: icons.puzzle },
      { id: "automations", label: "Automations", icon: icons.timer },
    ],
  },
  {
    title: "System",
    items: [
      { id: "config", label: "Config", icon: icons.settings },
      { id: "security", label: "Security", icon: icons.shield },
      { id: "nodes", label: "Nodes", icon: icons.server },
      { id: "usage", label: "Usage", icon: icons["bar-chart"] },
      { id: "metrics", label: "Observability", icon: icons["chart-line"] },
      { id: "logs", label: "Logs", icon: icons.terminal },
    ],
  },
];

@customElement("hu-sidebar")
export class ScSidebar extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      width: var(--hu-sidebar-width);
      min-width: var(--hu-sidebar-width);
      background: color-mix(
        in srgb,
        var(--hu-bg-surface) var(--hu-glass-standard-bg-opacity, 8%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      border-right: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 10%),
          transparent
        );
      transition:
        width var(--hu-duration-normal) var(--hu-spring-out, cubic-bezier(0.34, 1.56, 0.64, 1)),
        min-width var(--hu-duration-normal) var(--hu-spring-out, cubic-bezier(0.34, 1.56, 0.64, 1));
      overflow: hidden;
    }

    :host([collapsed]) {
      width: var(--hu-sidebar-collapsed);
      min-width: var(--hu-sidebar-collapsed);
    }

    @media (prefers-reduced-transparency: reduce) {
      :host {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-surface-container);
        border-right: 1px solid var(--hu-border);
      }
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-md);
      flex-shrink: 0;
      position: relative;
    }

    .header::after {
      content: "";
      position: absolute;
      bottom: 0;
      left: var(--hu-space-md);
      right: var(--hu-space-md);
      height: 1px;
      background: linear-gradient(
        90deg,
        transparent,
        color-mix(in srgb, var(--hu-accent) 20%, transparent),
        transparent
      );
      opacity: 0;
      transition: opacity var(--hu-duration-slow) var(--hu-ease-out);
    }

    .header:has(.connected-pulse)::after {
      opacity: 1;
      animation: hu-ambient-sweep var(--hu-duration-ambient, 25s) linear infinite;
      background-size: 200% 100%;
    }

    @keyframes hu-ambient-sweep {
      0% {
        background-position: -200% center;
      }
      100% {
        background-position: 200% center;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .header::after {
        animation: none !important;
      }
    }

    .brand-wrap {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      min-width: 0;
    }

    .connected-pulse {
      flex-shrink: 0;
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: var(--hu-radius-full);
      background: var(--hu-accent);
      opacity: 0.9;
      animation: hu-pulse-ambient var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    @keyframes hu-pulse-ambient {
      0%,
      100% {
        opacity: 0.9;
        transform: scale(1);
      }
      50% {
        opacity: 0.5;
        transform: scale(1.15);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .connected-pulse {
        animation: none;
      }
    }

    .logo {
      flex-shrink: 0;
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
      color: var(--hu-accent-text, var(--hu-accent));
    }

    .logo svg {
      width: 100%;
      height: 100%;
    }

    .brand {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      white-space: nowrap;
      overflow: hidden;
      opacity: 1;
      transition: opacity var(--hu-duration-normal) var(--hu-ease-out);
    }

    :host([collapsed]) .brand-wrap {
      opacity: 0;
      width: 0;
      overflow: hidden;
      pointer-events: none;
    }

    .nav {
      flex: 1;
      overflow-y: auto;
      overflow-x: hidden;
      padding: var(--hu-space-sm);
    }

    .section {
      margin-bottom: var(--hu-space-md);
    }

    .section-title {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      letter-spacing: var(--hu-tracking-xs);
      text-transform: uppercase;
      color: var(--hu-text-muted);
      padding: var(--hu-space-xs) var(--hu-space-md);
      margin-bottom: var(--hu-space-xs);
      white-space: nowrap;
      overflow: hidden;
      transition: opacity var(--hu-duration-normal) var(--hu-ease-out);
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
      gap: var(--hu-space-sm);
      width: 100%;
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: transparent;
      border: none;
      border-left: 3px solid transparent;
      border-radius: var(--hu-radius-sm);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast),
        color var(--hu-duration-fast),
        border-color var(--hu-duration-fast),
        transform var(--hu-duration-normal) var(--hu-spring-out);
      margin-bottom: var(--hu-space-xs);
      text-align: left;
      font-family: var(--hu-font);
    }

    .nav-item:active {
      transform: scale(0.97);
    }

    .nav-item:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: calc(-1 * var(--hu-focus-ring-width));
      border-radius: var(--hu-radius);
    }

    .nav-item:hover:not([aria-current]) {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    .nav-item[aria-current] {
      background: var(--hu-accent-subtle);
      border-left: 3px solid var(--hu-accent);
      color: var(--hu-accent-text, var(--hu-accent));
    }

    :host([collapsed]) .nav-item {
      justify-content: center;
      padding: var(--hu-space-sm);
    }

    :host([collapsed]) .nav-item .label {
      display: none;
    }

    .nav-item .icon {
      flex-shrink: 0;
      width: var(--hu-icon-md);
      height: var(--hu-icon-md);
    }

    .nav-item .icon svg,
    .nav-item svg {
      flex-shrink: 0;
      width: 100%;
      height: 100%;
    }

    .footer {
      flex-shrink: 0;
      padding: var(--hu-space-md);
      border-top: 1px solid var(--hu-border-subtle);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
    }

    .status-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    :host([collapsed]) .status-row .status-label {
      display: none;
    }

    :host([collapsed]) .status-row {
      justify-content: center;
    }

    .theme-toggle {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      width: 100%;
      padding: var(--hu-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      transition:
        background var(--hu-duration-fast),
        color var(--hu-duration-fast);
    }

    .theme-toggle:hover {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    .theme-toggle .icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      flex-shrink: 0;
    }

    .theme-toggle .icon svg {
      width: 100%;
      height: 100%;
    }

    :host([collapsed]) .theme-toggle .label {
      display: none;
    }

    .collapse-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 100%;
      padding: var(--hu-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast),
        color var(--hu-duration-fast),
        transform var(--hu-duration-normal) var(--hu-ease-out);
    }

    .collapse-btn:active {
      transform: scale(0.97);
    }

    .collapse-btn:hover {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    :host([collapsed]) .collapse-btn .icon {
      transform: rotate(180deg);
    }

    .collapse-btn .icon {
      width: var(--hu-icon-md);
      height: var(--hu-icon-md);
    }

    .collapse-btn .icon svg {
      width: 100%;
      height: 100%;
    }

    :host([collapsed]) .collapse-btn .label {
      display: none;
    }

    .collapse-btn .label {
      margin-left: var(--hu-space-sm);
    }
  `;

  @property({ type: String }) activeTab = "overview";
  @property({ type: Boolean, reflect: true }) collapsed = false;
  @property({ type: String }) connectionStatus: ConnectionStatus = "disconnected";
  @property({ type: String }) theme: "system" | "dark" | "light" = (() => {
    try {
      return (localStorage.getItem("hu-theme") as "system" | "dark" | "light") || "system";
    } catch {
      return "system";
    }
  })();

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
          <div class="brand-wrap">
            ${this.connectionStatus === "connected"
              ? html`<span
                  class="connected-pulse"
                  aria-hidden="true"
                  title="Gateway connected"
                ></span>`
              : nothing}
            <span class="brand">h-uman</span>
          </div>
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
            <hu-status-dot status=${this.connectionStatus}></hu-status-dot>
            <span class="status-label">${this.connectionStatus}</span>
          </div>
          <button
            class="theme-toggle"
            aria-label="Toggle theme"
            title=${this.collapsed ? this._themeLabel() : undefined}
            @click=${this._cycleTheme}
          >
            <span class="icon">${this._themeIcon()}</span>
            <span class="label">${this._themeLabel()}</span>
          </button>
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

  private _themeIcon() {
    if (this.theme === "dark")
      return html`<svg fill="currentColor" viewBox="0 0 256 256">
        <path
          d="M233.54,142.23a8,8,0,0,0-8-2,88.08,88.08,0,0,1-109.8-109.8,8,8,0,0,0-10-10,104.84,104.84,0,0,0-52.91,37A104,104,0,0,0,136,224a103.09,103.09,0,0,0,62.52-20.88,104.84,104.84,0,0,0,37-52.91A8,8,0,0,0,233.54,142.23ZM188.9,190.36A88,88,0,0,1,65.64,67.09,89,89,0,0,1,98,41.64a104.11,104.11,0,0,0,116.36,116.36A89,89,0,0,1,188.9,190.36Z"
        />
      </svg>`;
    if (this.theme === "light")
      return html`<svg fill="currentColor" viewBox="0 0 256 256">
        <path
          d="M120,40V16a8,8,0,0,1,16,0V40a8,8,0,0,1-16,0Zm72,88a64,64,0,1,1-64-64A64.07,64.07,0,0,1,192,128Zm-16,0a48,48,0,1,0-48,48A48.05,48.05,0,0,0,176,128ZM58.34,69.66A8,8,0,0,0,69.66,58.34l-16-16A8,8,0,0,0,42.34,53.66Zm0,116.68-16,16a8,8,0,0,0,11.32,11.32l16-16a8,8,0,0,0-11.32-11.32ZM192,72a8,8,0,0,0,5.66-2.34l16-16a8,8,0,0,0-11.32-11.32l-16,16A8,8,0,0,0,192,72Zm5.66,114.34a8,8,0,0,0-11.32,11.32l16,16a8,8,0,0,0,11.32-11.32ZM48,128a8,8,0,0,0-8-8H16a8,8,0,0,0,0,16H40A8,8,0,0,0,48,128Zm80,80a8,8,0,0,0-8,8v24a8,8,0,0,0,16,0V216A8,8,0,0,0,128,208Zm112-88H216a8,8,0,0,0,0,16h24a8,8,0,0,0,0-16Z"
        />
      </svg>`;
    return html`<svg fill="currentColor" viewBox="0 0 256 256">
      <path
        d="M128,24A104,104,0,1,0,232,128,104.11,104.11,0,0,0,128,24Zm0,192a88,88,0,0,1,0-176Z"
      />
    </svg>`;
  }

  private _themeLabel() {
    if (this.theme === "dark") return "Dark";
    if (this.theme === "light") return "Light";
    return "System";
  }

  private _cycleTheme = () => {
    const order: Array<"system" | "light" | "dark"> = ["system", "light", "dark"];
    const idx = order.indexOf(this.theme);
    this.theme = order[(idx + 1) % order.length];
    localStorage.setItem("hu-theme", this.theme);
    this._applyTheme();
  };

  private _applyTheme() {
    const root = document.documentElement;
    if (this.theme === "system") {
      root.removeAttribute("data-theme");
    } else {
      root.setAttribute("data-theme", this.theme);
    }
  }

  override connectedCallback() {
    super.connectedCallback();
    this._applyTheme();
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

declare global {
  interface HTMLElementTagNameMap {
    "hu-sidebar": ScSidebar;
  }
}
