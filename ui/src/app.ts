import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { GatewayClient, GatewayStatus } from "./gateway.js";
import { GatewayClient as GatewayClientClass } from "./gateway.js";
import { DemoGatewayClient } from "./demo-gateway.js";
import { setGateway } from "./gateway-provider.js";
import { dynamicLight } from "./lib/dynamic-light.js";
import { icons } from "./icons.js";
import "./components/floating-mic.js";
import "./components/sidebar.js";
import "./components/command-palette.js";
import "./components/sc-shortcut-overlay.js";
import "./components/sc-error-boundary.js";
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
  | "security"
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
  "security",
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
  security: () => import("./views/security-view.js"),
  logs: () => import("./views/logs-view.js"),
};

const loadedViews = new Set<TabId>(["overview"]);

const MOBILE_TABS: { id: TabId; label: string; icon: ReturnType<typeof html> }[] = [
  { id: "overview", label: "Home", icon: icons.grid },
  { id: "chat", label: "Chat", icon: icons["message-square"] },
  { id: "agents", label: "Agents", icon: icons.zap },
  { id: "config", label: "Config", icon: icons.settings },
];

const MORE_TABS: { id: TabId; label: string; icon: ReturnType<typeof html> }[] = [
  { id: "tools", label: "Tools", icon: icons.wrench },
  { id: "sessions", label: "Sessions", icon: icons.clock },
  { id: "models", label: "Models", icon: icons.cpu },
  { id: "channels", label: "Channels", icon: icons["message-square"] },
  { id: "cron", label: "Cron", icon: icons.clock },
  { id: "skills", label: "Skills", icon: icons.zap },
  { id: "voice", label: "Voice", icon: icons.mic },
  { id: "nodes", label: "Nodes", icon: icons.server },
  { id: "usage", label: "Usage", icon: icons["bar-chart"] },
  { id: "security", label: "Security", icon: icons.shield },
  { id: "logs", label: "Logs", icon: icons.terminal },
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
      padding: var(--sc-space-2xl);
      background: var(--sc-bg);
      view-transition-name: main-content;
      content-visibility: auto;
      contain-intrinsic-size: auto 100vh;
      position: relative;
      outline: none;
    }

    main:focus-visible {
      outline: var(--sc-focus-ring-width, 2px) solid var(--sc-focus-ring);
      outline-offset: calc(-1 * var(--sc-focus-ring-width, 2px));
    }

    @keyframes sc-ambient-drift {
      0%,
      100% {
        transform: translate(0, 0);
      }
      25% {
        transform: translate(2%, 1%);
      }
      50% {
        transform: translate(-1%, 2%);
      }
      75% {
        transform: translate(1%, -1%);
      }
    }

    /* Ambient ocean lighting — living teal atmosphere with subtle drift */
    main::before {
      content: "";
      position: fixed;
      top: -15%;
      left: -10%;
      width: 55%;
      height: 55%;
      background: radial-gradient(
        ellipse 70% 60% at 40% 30%,
        color-mix(in srgb, var(--sc-accent) 7%, transparent),
        transparent 70%
      );
      pointer-events: none;
      z-index: 0;
      animation: sc-ambient-drift 25s ease-in-out infinite;
    }
    main::after {
      content: "";
      position: fixed;
      bottom: -20%;
      right: -10%;
      width: 50%;
      height: 50%;
      background: radial-gradient(
        ellipse 60% 50% at 60% 60%,
        color-mix(in srgb, var(--sc-accent) 4%, transparent),
        transparent 70%
      );
      pointer-events: none;
      z-index: 0;
      animation: sc-ambient-drift 30s ease-in-out infinite reverse;
    }

    .view-enter {
      animation: sc-view-enter var(--sc-duration-moderate) var(--sc-spring-out) both;
    }
    @keyframes sc-view-enter {
      from {
        opacity: 0;
        transform: translateY(8px) scale(0.995);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .view-enter {
        animation: none;
      }
    }

    .disconnect-banner {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: var(--sc-error);
      color: var(--sc-on-accent, #fff);
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      animation: sc-slide-down var(--sc-duration-normal) var(--sc-ease-out);
    }
    @keyframes sc-slide-down {
      from {
        transform: translateY(-100%);
      }
      to {
        transform: translateY(0);
      }
    }
    .disconnect-banner button {
      background: color-mix(in srgb, var(--sc-on-accent, #fff) 20%, transparent);
      border: 1px solid color-mix(in srgb, var(--sc-on-accent, #fff) 40%, transparent);
      color: var(--sc-on-accent, #fff);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      cursor: pointer;
      transition: background var(--sc-duration-fast);
    }
    .disconnect-banner button:hover {
      background: color-mix(in srgb, var(--sc-on-accent, #fff) 35%, transparent);
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
        background: color-mix(in srgb, var(--sc-bg-surface) 82%, transparent);
        backdrop-filter: blur(20px) saturate(180%);
        -webkit-backdrop-filter: blur(20px) saturate(180%);
        box-shadow: var(--sc-shadow-sm);
        padding: var(--sc-space-xs) 0;
        padding-bottom: env(safe-area-inset-bottom, 0);
      }
      .mobile-tab {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: var(--sc-space-2xs);
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
        color: var(--sc-accent-text, var(--sc-accent));
      }
      .mobile-tab .icon {
        width: 1.25rem;
        height: 1.25rem;
        line-height: 1;
      }
      .mobile-tab .icon svg {
        width: 100%;
        height: 100%;
      }
    }

    .more-backdrop {
      position: fixed;
      inset: 0;
      z-index: 9998;
      background: color-mix(in srgb, var(--sc-bg) 40%, transparent);
      backdrop-filter: blur(4px);
      -webkit-backdrop-filter: blur(4px);
    }
    .more-sheet {
      position: fixed;
      bottom: 0;
      left: 0;
      right: 0;
      z-index: 9999;
      background: var(--sc-bg-surface);
      border-radius: var(--sc-radius-xl) var(--sc-radius-xl) 0 0;
      box-shadow: var(--sc-shadow-lg);
      padding: var(--sc-space-md) var(--sc-space-md)
        calc(var(--sc-space-lg) + env(safe-area-inset-bottom, 0));
      animation: sc-sheet-up var(--sc-duration-normal) var(--sc-ease-out);
      max-height: 70vh;
      overflow-y: auto;
    }
    @keyframes sc-sheet-up {
      from {
        transform: translateY(100%);
      }
      to {
        transform: translateY(0);
      }
    }
    .more-sheet-handle {
      width: 36px;
      height: 4px;
      background: var(--sc-border);
      border-radius: 2px;
      margin: 0 auto var(--sc-space-md);
    }
    .more-sheet-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--sc-space-sm);
    }
    .more-item {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-sm);
      border-radius: var(--sc-radius);
      background: transparent;
      border: none;
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      cursor: pointer;
      transition: background var(--sc-duration-fast);
    }
    .more-item:hover {
      background: var(--sc-bg-elevated);
    }
    .more-item .more-icon {
      width: 24px;
      height: 24px;
    }
    .more-item .more-icon svg {
      width: 100%;
      height: 100%;
    }
  `;

  @state() private tab: TabId = "overview";
  @state() private chatSessionKey = "default";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private sidebarCollapsed = false;
  @state() private commandPaletteOpen = false;
  @state() private shortcutOverlayOpen = false;
  @state() private moreSheetOpen = false;
  @state() private _viewError: Error | null = null;

  gateway: GatewayClient | null = null;
  private _keyHandler = this._onGlobalKey.bind(this);
  private _hashHandler = this._onHashChange.bind(this);

  private get _isDemo(): boolean {
    return new URLSearchParams(window.location.search).has("demo");
  }

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway = this._isDemo
      ? (new DemoGatewayClient() as unknown as GatewayClient)
      : new GatewayClientClass();
    setGateway(this.gateway);
    this.gateway.addEventListener("status", ((e: CustomEvent<GatewayStatus>) => {
      this.connectionStatus = e.detail;
    }) as EventListener);

    this.sidebarCollapsed = localStorage.getItem(SIDEBAR_KEY) === "true";
    document.addEventListener("keydown", this._keyHandler);
    window.addEventListener("hashchange", this._hashHandler);
    this._onHashChange();
    dynamicLight.start();

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
    dynamicLight.stop();
    this.gateway?.disconnect();
  }

  private _onHashChange(): void {
    const hash = window.location.hash.replace("#", "");
    if (!hash) return;
    const [tabPart, ...rest] = hash.split(":");
    const sessionPart = rest.join(":");
    if (VALID_TABS.includes(tabPart as TabId)) {
      const targetTab = tabPart as TabId;
      if (targetTab === "chat" && sessionPart) {
        this.chatSessionKey = sessionPart;
      }
      this._ensureLoaded(targetTab);
      if (!document.startViewTransition) {
        this.tab = targetTab;
        return;
      }
      document.startViewTransition(() => {
        this.tab = targetTab;
        return this.updateComplete;
      });
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
    if (e.key === "?") {
      e.preventDefault();
      this.shortcutOverlayOpen = !this.shortcutOverlayOpen;
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
    const hash = tab === "chat" ? `#chat:${this.chatSessionKey}` : `#${tab}`;
    window.history.replaceState(null, "", hash);
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
    } else if (action === "refresh") {
      const view = this.shadowRoot?.querySelector("main")?.firstElementChild;
      if (view && "load" in view && typeof (view as { load: () => void }).load === "function") {
        (view as { load: () => void }).load();
      }
    }
  }

  override render() {
    return html`
      ${this.connectionStatus === "disconnected"
        ? html`<div class="disconnect-banner" role="alert">
            Disconnected from server
            <button @click=${this._reconnect}>Reconnect</button>
          </div>`
        : nothing}
      <div class="layout ${this.sidebarCollapsed ? "collapsed" : ""}">
        <sc-sidebar
          .activeTab=${this.tab}
          ?collapsed=${this.sidebarCollapsed}
          .connectionStatus=${this.connectionStatus}
          @tab-change=${this._onTabChange}
          @toggle-collapse=${() => this._toggleSidebar()}
        ></sc-sidebar>

        <main tabindex="0">
          <div class="view-enter">
            <sc-error-boundary .error=${this._viewError} @retry=${this._onViewRetry}>
              ${this._renderWrappedView()}
            </sc-error-boundary>
          </div>
        </main>

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
          <button
            class="mobile-tab ${this.moreSheetOpen ? "active" : ""}"
            @click=${() => (this.moreSheetOpen = !this.moreSheetOpen)}
            aria-label="More"
          >
            <span class="icon">${icons.grid}</span>
            <span>More</span>
          </button>
        </nav>
      </div>
      ${this.moreSheetOpen
        ? html`
            <div class="more-backdrop" @click=${() => (this.moreSheetOpen = false)}></div>
            <div class="more-sheet" role="dialog" aria-label="More views">
              <div class="more-sheet-handle"></div>
              <div class="more-sheet-grid">
                ${MORE_TABS.map(
                  (t) => html`
                    <button
                      class="more-item"
                      @click=${() => {
                        this.moreSheetOpen = false;
                        this._switchTab(t.id);
                      }}
                    >
                      <span class="more-icon">${t.icon}</span>
                      <span>${t.label}</span>
                    </button>
                  `,
                )}
              </div>
            </div>
          `
        : nothing}

      <sc-command-palette
        .open=${this.commandPaletteOpen}
        @execute=${this._onCommandExecute}
        @close=${() => {
          this.commandPaletteOpen = false;
        }}
      ></sc-command-palette>

      <sc-shortcut-overlay
        .open=${this.shortcutOverlayOpen}
        @close=${() => {
          this.shortcutOverlayOpen = false;
        }}
      ></sc-shortcut-overlay>

      <sc-floating-mic></sc-floating-mic>
    `;
  }

  private _renderWrappedView() {
    if (this._viewError) return null;
    try {
      return this._renderView();
    } catch (e) {
      const err = e instanceof Error ? e : new Error(String(e));
      this._viewError = err;
      return null;
    }
  }

  private _onViewRetry(): void {
    this._viewError = null;
    this.requestUpdate();
  }

  private _reconnect(): void {
    if (!this.gateway) return;
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    const wsUrl = `${proto}//${window.location.host}/ws`;
    this.gateway.connect(wsUrl);
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
      case "security":
        return html`<sc-security-view></sc-security-view>`;
      case "logs":
        return html`<sc-logs-view></sc-logs-view>`;
      default:
        return html`<sc-overview-view></sc-overview-view>`;
    }
  }
}
