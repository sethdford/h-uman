import { LitElement, html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { GatewayClient, GatewayStatus } from "./gateway.js";
import { GatewayClient as GatewayClientClass } from "./gateway.js";
import { setGateway } from "./gateway-provider.js";
import "./components/floating-mic.js";
import "./components/sidebar.js";
import "./components/command-palette.js";
import "./views/overview-view.js";

type TabId =
  | "overview"
  | "chat"
  | "agents"
  | "sessions"
  | "models"
  | "config"
  | "tools"
  | "channels"
  | "cron"
  | "skills"
  | "voice"
  | "nodes"
  | "usage"
  | "logs";

const VALID_TABS: TabId[] = [
  "overview",
  "chat",
  "agents",
  "sessions",
  "models",
  "config",
  "tools",
  "channels",
  "cron",
  "skills",
  "voice",
  "nodes",
  "usage",
  "logs",
];

const SIDEBAR_KEY = "sc-sidebar-collapsed";

const VIEW_IMPORTS: Record<TabId, () => Promise<unknown>> = {
  overview: () => Promise.resolve(),
  chat: () => import("./views/chat-view.js"),
  agents: () => import("./views/agents-view.js"),
  sessions: () => import("./views/sessions-view.js"),
  models: () => import("./views/models-view.js"),
  config: () => import("./views/config-view.js"),
  tools: () => import("./views/tools-view.js"),
  channels: () => import("./views/channels-view.js"),
  cron: () => import("./views/cron-view.js"),
  skills: () => import("./views/skills-view.js"),
  voice: () => import("./views/voice-view.js"),
  nodes: () => import("./views/nodes-view.js"),
  usage: () => import("./views/usage-view.js"),
  logs: () => import("./views/logs-view.js"),
};

const loadedViews = new Set<TabId>(["overview"]);

const MOBILE_TABS: { id: TabId; label: string; icon: string }[] = [
  { id: "overview", label: "Home", icon: "⊞" },
  { id: "chat", label: "Chat", icon: "💬" },
  { id: "agents", label: "Agents", icon: "⚡" },
  { id: "config", label: "Config", icon: "⚙" },
  { id: "tools", label: "Tools", icon: "🔧" },
];

@customElement("sc-app")
export class ScApp extends LitElement {
  static override styles = css`
    :host {
      display: block;
      height: 100vh;
      font-family: var(--sc-font);
    }

    .layout {
      display: grid;
      grid-template-columns: var(--sc-sidebar-width) 1fr;
      height: 100%;
      transition: grid-template-columns var(--sc-duration-normal) var(--sc-ease-out);
    }

    .layout.collapsed {
      grid-template-columns: var(--sc-sidebar-collapsed) 1fr;
    }

    main {
      overflow: auto;
      padding: var(--sc-space-lg);
      background: var(--sc-bg);
      view-transition-name: main-content;
      content-visibility: auto;
      contain-intrinsic-size: auto 100vh;
    }

    .mobile-nav {
      display: none;
    }

    @media (max-width: 768px) {
      .layout {
        grid-template-columns: 1fr;
        grid-template-rows: 1fr auto;
      }
      .layout.collapsed {
        grid-template-columns: 1fr;
      }
      sc-sidebar {
        display: none;
      }
      main {
        padding: var(--sc-space-md);
      }
      .mobile-nav {
        display: flex;
        align-items: center;
        justify-content: space-around;
        background: var(--sc-bg-surface);
        border-top: 1px solid var(--sc-border-subtle);
        padding: var(--sc-space-xs) 0;
        padding-bottom: env(safe-area-inset-bottom, 0);
      }
      .mobile-tab {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 2px;
        padding: var(--sc-space-xs) var(--sc-space-sm);
        background: transparent;
        border: none;
        color: var(--sc-text-muted);
        font-size: var(--sc-text-xs);
        font-family: var(--sc-font);
        cursor: pointer;
        min-width: 44px;
        min-height: 44px;
        border-radius: var(--sc-radius-sm);
        transition: color var(--sc-duration-fast);
      }
      .mobile-tab:hover,
      .mobile-tab.active {
        color: var(--sc-accent);
      }
      .mobile-tab .icon {
        font-size: 1.25rem;
        line-height: 1;
      }
    }
  `;

  @state() private tab: TabId = "overview";
  @state() private chatSessionKey = "default";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private sidebarCollapsed = false;
  @state() private commandPaletteOpen = false;

  gateway: GatewayClient | null = null;
  private _keyHandler = this._onGlobalKey.bind(this);
  private _hashHandler = this._onHashChange.bind(this);

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway = new GatewayClientClass();
    setGateway(this.gateway);
    this.gateway.addEventListener("status", ((e: CustomEvent<GatewayStatus>) => {
      this.connectionStatus = e.detail;
    }) as EventListener);

    this.sidebarCollapsed = localStorage.getItem(SIDEBAR_KEY) === "true";
    document.addEventListener("keydown", this._keyHandler);
    window.addEventListener("hashchange", this._hashHandler);
    this._onHashChange();

    const wsUrl =
      typeof window !== "undefined" &&
      (window as unknown as { __VITE_WS_PROXY__?: string }).__VITE_WS_PROXY__
        ? (window as unknown as { __VITE_WS_PROXY__: string }).__VITE_WS_PROXY__
        : (() => {
            const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
            return `${proto}//${window.location.host}/ws`;
          })();
    this.gateway.connect(wsUrl);

    this.addEventListener("navigate", ((e: CustomEvent<string>) => {
      const raw = e.detail as string;
      const [tabPart, sessionPart] = raw.includes(":")
        ? (raw.split(":") as [string, string])
        : [raw, undefined];
      const target = tabPart as TabId;
      if (VALID_TABS.includes(target)) {
        this._switchTab(target);
        if (target === "chat") {
          this.chatSessionKey = sessionPart ?? "default";
        }
      }
    }) as EventListener);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    document.removeEventListener("keydown", this._keyHandler);
    window.removeEventListener("hashchange", this._hashHandler);
    this.gateway?.disconnect();
  }

  private _onHashChange(): void {
    const hash = window.location.hash.replace("#", "");
    if (hash && VALID_TABS.includes(hash as TabId)) {
      this._ensureLoaded(hash as TabId);
      this.tab = hash as TabId;
    }
  }

  private _onGlobalKey(e: KeyboardEvent): void {
    const mod = e.metaKey || e.ctrlKey;
    if (mod && e.key === "k") {
      e.preventDefault();
      this.commandPaletteOpen = !this.commandPaletteOpen;
    }
    if (mod && e.key === "b") {
      e.preventDefault();
      this._toggleSidebar();
    }
  }

  private _toggleSidebar(): void {
    this.sidebarCollapsed = !this.sidebarCollapsed;
    localStorage.setItem(SIDEBAR_KEY, String(this.sidebarCollapsed));
  }

  private async _ensureLoaded(tab: TabId): Promise<void> {
    if (loadedViews.has(tab)) return;
    await VIEW_IMPORTS[tab]();
    loadedViews.add(tab);
  }

  private async _switchTab(tab: TabId): Promise<void> {
    if (this.tab === tab) return;
    await this._ensureLoaded(tab);
    window.history.replaceState(null, "", `#${tab}`);
    if (!document.startViewTransition) {
      this.tab = tab;
      return;
    }
    document.startViewTransition(() => {
      this.tab = tab;
      return this.updateComplete;
    });
  }

  private _onTabChange(e: CustomEvent<string>): void {
    const target = e.detail as TabId;
    if (VALID_TABS.includes(target)) {
      this._switchTab(target);
    }
  }

  private _onCommandExecute(e: CustomEvent<{ action: string; id: string }>): void {
    this.commandPaletteOpen = false;
    const { action, id } = e.detail;
    if (action === "navigate" && VALID_TABS.includes(id as TabId)) {
      this._switchTab(id as TabId);
    } else if (action === "toggle-sidebar") {
      this._toggleSidebar();
    }
  }

  override render() {
    return html`
      <div class="layout ${this.sidebarCollapsed ? "collapsed" : ""}">
        <sc-sidebar
          .activeTab=${this.tab}
          ?collapsed=${this.sidebarCollapsed}
          .connectionStatus=${this.connectionStatus}
          @tab-change=${this._onTabChange}
          @toggle-collapse=${() => this._toggleSidebar()}
        ></sc-sidebar>

        <main>${this._renderView()}</main>

        <nav class="mobile-nav" aria-label="Mobile navigation">
          ${MOBILE_TABS.map(
            (t) => html`
              <button
                class="mobile-tab ${this.tab === t.id ? "active" : ""}"
                @click=${() => this._switchTab(t.id)}
                aria-label=${t.label}
              >
                <span class="icon">${t.icon}</span>
                <span>${t.label}</span>
              </button>
            `,
          )}
        </nav>
      </div>

      <sc-command-palette
        .open=${this.commandPaletteOpen}
        @execute=${this._onCommandExecute}
        @close=${() => {
          this.commandPaletteOpen = false;
        }}
      ></sc-command-palette>

      <sc-floating-mic></sc-floating-mic>
    `;
  }

  private _renderView() {
    switch (this.tab) {
      case "overview":
        return html`<sc-overview-view></sc-overview-view>`;
      case "chat":
        return html`<sc-chat-view .sessionKey=${this.chatSessionKey}></sc-chat-view>`;
      case "agents":
        return html`<sc-agents-view></sc-agents-view>`;
      case "sessions":
        return html`<sc-sessions-view></sc-sessions-view>`;
      case "models":
        return html`<sc-models-view></sc-models-view>`;
      case "config":
        return html`<sc-config-view></sc-config-view>`;
      case "tools":
        return html`<sc-tools-view></sc-tools-view>`;
      case "channels":
        return html`<sc-channels-view></sc-channels-view>`;
      case "cron":
        return html`<sc-cron-view></sc-cron-view>`;
      case "skills":
        return html`<sc-skills-view></sc-skills-view>`;
      case "voice":
        return html`<sc-voice-view></sc-voice-view>`;
      case "nodes":
        return html`<sc-nodes-view></sc-nodes-view>`;
      case "usage":
        return html`<sc-usage-view></sc-usage-view>`;
      case "logs":
        return html`<sc-logs-view></sc-logs-view>`;
      default:
        return html`<sc-overview-view></sc-overview-view>`;
    }
  }
}
