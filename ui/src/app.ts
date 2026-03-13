import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import type { GatewayClient, GatewayStatus } from "./gateway.js";
import { GatewayClient as GatewayClientClass } from "./gateway.js";
import { DemoGatewayClient } from "./demo-gateway.js";
import { setGateway } from "./gateway-provider.js";
import { AUTH_FAILED } from "./gateway-aware.js";
import { dynamicLight } from "./lib/dynamic-light.js";
import { icons } from "./icons.js";
import "./components/floating-mic.js";
import "./components/sidebar.js";
import "./components/command-palette.js";
import "./components/hu-shortcut-overlay.js";
import "./components/hu-error-boundary.js";
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
  | "automations"
  | "skills"
  | "voice"
  | "nodes"
  | "usage"
  | "metrics"
  | "memory"
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
  "automations",
  "skills",
  "voice",
  "nodes",
  "usage",
  "metrics",
  "memory",
  "security",
  "logs",
];

const SIDEBAR_KEY = "hu-sidebar-collapsed";

const VIEW_IMPORTS: Record<TabId, () => Promise<unknown>> = {
  overview: () => Promise.resolve(),
  chat: () => import("./views/chat-view.js"),
  agents: () => import("./views/agents-view.js"),
  sessions: () => import("./views/sessions-view.js"),
  models: () => import("./views/models-view.js"),
  config: () => import("./views/config-view.js"),
  tools: () => import("./views/tools-view.js"),
  channels: () => import("./views/channels-view.js"),
  automations: () => import("./views/automations-view.js"),
  skills: () => import("./views/skills-view.js"),
  voice: () => import("./views/voice-view.js"),
  nodes: () => import("./views/nodes-view.js"),
  usage: () => import("./views/usage-view.js"),
  metrics: () => import("./views/metrics-view.js"),
  memory: () => import("./views/memory-view.js"),
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
  { id: "models", label: "Models", icon: icons.cpu },
  { id: "channels", label: "Channels", icon: icons["message-square"] },
  { id: "automations", label: "Automations", icon: icons.timer },
  { id: "skills", label: "Skills", icon: icons.puzzle },
  { id: "voice", label: "Voice", icon: icons.mic },
  { id: "nodes", label: "Nodes", icon: icons.server },
  { id: "usage", label: "Usage", icon: icons["bar-chart"] },
  { id: "metrics", label: "Observability", icon: icons["chart-line"] },
  { id: "security", label: "Security", icon: icons.shield },
  { id: "logs", label: "Logs", icon: icons.terminal },
];

@customElement("hu-app")
export class ScApp extends LitElement {
  static override styles = css`
    :host {
      display: block;
      height: 100vh;
      font-family: var(--hu-font);
    }
    .hu-skip-link {
      position: absolute;
      top: -100%;
      left: var(--hu-space-md);
      z-index: 9999;
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: var(--hu-accent);
      color: var(--hu-on-accent);
      border-radius: var(--hu-radius-sm);
      text-decoration: none;
      font-weight: 600;
    }
    .hu-skip-link:focus {
      top: var(--hu-space-md);
    }

    .layout {
      display: grid;
      grid-template-columns: var(--hu-sidebar-width) 1fr;
      height: 100%;
      transition: grid-template-columns var(--hu-duration-normal) var(--hu-ease-out);
    }

    .layout.collapsed {
      grid-template-columns: var(--hu-sidebar-collapsed) 1fr;
    }

    main {
      display: flex;
      flex-direction: column;
      overflow: auto;
      padding: var(--hu-space-2xl);
      background: var(--hu-bg);
      view-transition-name: main-content;
      position: relative;
      outline: none;
    }

    main:focus-visible {
      outline: var(--hu-focus-ring-width, 2px) solid var(--hu-focus-ring);
      outline-offset: calc(-1 * var(--hu-focus-ring-width, 2px));
    }

    @keyframes hu-ambient-drift {
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
        color-mix(in srgb, var(--hu-accent) 7%, transparent),
        transparent 70%
      );
      pointer-events: none;
      z-index: 0;
      animation: hu-ambient-drift var(--hu-duration-ambient) var(--hu-ease-in-out) infinite;
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
        color-mix(in srgb, var(--hu-accent) 4%, transparent),
        transparent 70%
      );
      pointer-events: none;
      z-index: 0;
      animation: hu-ambient-drift var(--hu-duration-ambient-slow) var(--hu-ease-in-out) infinite
        reverse;
    }

    .view-enter {
      flex: 1;
      min-height: 0;
      display: flex;
      flex-direction: column;
      animation: hu-view-enter var(--hu-duration-moderate) var(--hu-spring-out) both;
    }
    @keyframes hu-view-enter {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-sm)) scale(0.995);
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
      gap: var(--hu-space-sm);
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: var(--hu-error);
      color: var(--hu-on-accent);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      animation: hu-slide-down var(--hu-duration-normal) var(--hu-ease-out);
    }
    @keyframes hu-slide-down {
      from {
        transform: translateY(-100%);
      }
      to {
        transform: translateY(0);
      }
    }
    .disconnect-banner button {
      background: color-mix(in srgb, var(--hu-on-accent) 20%, transparent);
      border: 1px solid color-mix(in srgb, var(--hu-on-accent) 40%, transparent);
      color: var(--hu-on-accent);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-sm);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      cursor: pointer;
      transition: background var(--hu-duration-fast);
    }
    .disconnect-banner button:hover {
      background: color-mix(in srgb, var(--hu-on-accent) 35%, transparent);
    }

    .mobile-nav {
      display: none;
    }

    @media (max-width: var(--hu-breakpoint-lg)) /* --hu-breakpoint-lg */ {
      .layout {
        grid-template-columns: 1fr;
        grid-template-rows: 1fr auto;
      }
      .layout.collapsed {
        grid-template-columns: 1fr;
      }
      hu-sidebar {
        display: none;
      }
      main {
        padding: var(--hu-space-md);
      }
      .mobile-nav {
        display: flex;
        align-items: center;
        justify-content: space-around;
        background: color-mix(in srgb, var(--hu-bg-surface) 82%, transparent);
        backdrop-filter: blur(var(--hu-glass-blur, 20px)) saturate(180%);
        -webkit-backdrop-filter: blur(var(--hu-glass-blur, 20px)) saturate(180%);
        box-shadow: var(--hu-shadow-sm);
        padding: var(--hu-space-xs) 0;
        padding-bottom: env(safe-area-inset-bottom, 0);
      }
      .mobile-tab {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: var(--hu-space-2xs);
        padding: var(--hu-space-xs) var(--hu-space-sm);
        background: transparent;
        border: none;
        color: var(--hu-text-muted);
        font-size: var(--hu-text-xs);
        font-family: var(--hu-font);
        cursor: pointer;
        min-width: 44px;
        min-height: 44px;
        border-radius: var(--hu-radius-sm);
        transition: color var(--hu-duration-fast);
      }
      .mobile-tab:hover,
      .mobile-tab.active {
        color: var(--hu-accent-text, var(--hu-accent));
      }
      .mobile-tab .icon {
        width: var(--hu-icon-md);
        height: var(--hu-icon-md);
        line-height: var(--hu-leading-none);
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
      background: color-mix(in srgb, var(--hu-bg) 40%, transparent);
      backdrop-filter: blur(var(--hu-blur-sm, 4px));
      -webkit-backdrop-filter: blur(var(--hu-blur-sm, 4px));
    }
    .more-sheet {
      position: fixed;
      bottom: 0;
      left: 0;
      right: 0;
      z-index: 9999;
      background: var(--hu-bg-surface);
      border-radius: var(--hu-radius-xl) var(--hu-radius-xl) 0 0;
      box-shadow: var(--hu-shadow-lg);
      padding: var(--hu-space-md) var(--hu-space-md)
        calc(var(--hu-space-lg) + env(safe-area-inset-bottom, 0));
      animation: hu-sheet-up var(--hu-duration-normal) var(--hu-ease-out);
      max-height: 70vh;
      overflow-y: auto;
    }
    @keyframes hu-sheet-up {
      from {
        transform: translateY(100%);
      }
      to {
        transform: translateY(0);
      }
    }
    .more-sheet-handle {
      width: var(--hu-space-2xl, 36px);
      height: var(--hu-space-2xs, 4px);
      background: var(--hu-border);
      border-radius: var(--hu-radius-sm, 2px);
      margin: 0 auto var(--hu-space-md);
    }
    .more-sheet-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--hu-space-sm);
    }
    .more-item {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-sm);
      border-radius: var(--hu-radius);
      background: transparent;
      border: none;
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      cursor: pointer;
      transition: background var(--hu-duration-fast);
    }
    .more-item:hover {
      background: var(--hu-hover-overlay);
    }
    .more-item .more-icon {
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
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
  @state() private _inFallbackWindow = false;

  gateway: GatewayClient | null = null;
  private _keyHandler = this._onGlobalKey.bind(this);
  private _hashHandler = this._onHashChange.bind(this);
  private _moreSheetKeyHandler = this._onMoreSheetKeyDown.bind(this);
  private _moreSheetPreviousElement: HTMLElement | null = null;
  private readonly _moreSheetFocusableSelector =
    'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
  private _statusHandler = ((e: CustomEvent<GatewayStatus>) => {
    this.connectionStatus = e.detail;
  }) as EventListener;
  private _fallbackTimer: ReturnType<typeof setTimeout> | null = null;
  private _authFailedHandler = (() => {
    if (!this._isDemo) this._switchToDemo();
  }) as EventListener;

  private get _isDemo(): boolean {
    return new URLSearchParams(window.location.search).has("demo");
  }

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway = this._isDemo
      ? (new DemoGatewayClient() as unknown as GatewayClient)
      : new GatewayClientClass();
    setGateway(this.gateway);
    this.gateway.addEventListener("status", this._statusHandler);

    this.sidebarCollapsed = localStorage.getItem(SIDEBAR_KEY) === "true";
    document.addEventListener("keydown", this._keyHandler);
    document.addEventListener(AUTH_FAILED, this._authFailedHandler);
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

    // Auto-fallback: if real gateway doesn't complete handshake within 2.5s, use demo.
    // A raw WebSocket open (e.g. to Vite's HMR server) is not enough — we need a
    // successful "connect" handshake proving this is a real human gateway.
    if (!this._isDemo) {
      this._inFallbackWindow = true;
      this._fallbackTimer = setTimeout(() => {
        this._switchToDemo();
      }, 2500);
      this.gateway.addEventListener("features", (() => {
        if (this._fallbackTimer) {
          clearTimeout(this._fallbackTimer);
          this._fallbackTimer = null;
          this._inFallbackWindow = false;
        }
      }) as EventListener);
    }

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

    this.addEventListener("hu-voice-transcribe", ((
      e: CustomEvent<{ audio: string; mimeType: string }>,
    ) => {
      if (!this.gateway) return;
      this.gateway
        .request<{ text?: string }>("voice.transcribe", {
          audio: e.detail.audio,
          mimeType: e.detail.mimeType,
        })
        .then((result) => {
          window.dispatchEvent(
            new CustomEvent("hu-voice-transcript-result", {
              detail: { text: result.text ?? "" },
            }),
          );
        })
        .catch(() => {
          window.dispatchEvent(
            new CustomEvent("hu-voice-transcript-result", {
              detail: { text: "" },
            }),
          );
        });
    }) as EventListener);
  }

  override updated(changedProperties: PropertyValues): void {
    if (changedProperties.has("moreSheetOpen")) {
      if (this.moreSheetOpen) {
        this._moreSheetPreviousElement = document.activeElement as HTMLElement | null;
        document.addEventListener("keydown", this._moreSheetKeyHandler);
        requestAnimationFrame(() => this._trapMoreSheetFocus());
      } else {
        document.removeEventListener("keydown", this._moreSheetKeyHandler);
        this._restoreMoreSheetFocus();
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    document.removeEventListener("keydown", this._moreSheetKeyHandler);
    document.removeEventListener("keydown", this._keyHandler);
    document.removeEventListener(AUTH_FAILED, this._authFailedHandler);
    window.removeEventListener("hashchange", this._hashHandler);
    dynamicLight.stop();
    if (this._fallbackTimer) {
      clearTimeout(this._fallbackTimer);
      this._fallbackTimer = null;
    }
    this.gateway?.removeEventListener("status", this._statusHandler);
    this.gateway?.disconnect();
  }

  private _getMoreSheetFocusable(): HTMLElement[] {
    const sheet = this.shadowRoot?.querySelector(".more-sheet");
    if (!sheet) return [];
    return Array.from(sheet.querySelectorAll<HTMLElement>(this._moreSheetFocusableSelector));
  }

  private _trapMoreSheetFocus(): void {
    const focusable = this._getMoreSheetFocusable();
    if (focusable.length > 0) focusable[0].focus();
  }

  private _restoreMoreSheetFocus(): void {
    const btn = this.shadowRoot?.querySelector("#more-tab-btn");
    if (btn instanceof HTMLElement && btn.focus) btn.focus();
    else if (this._moreSheetPreviousElement?.focus) this._moreSheetPreviousElement.focus();
  }

  private _onMoreSheetKeyDown(e: KeyboardEvent): void {
    if (!this.moreSheetOpen) return;
    if (e.key === "Escape") {
      e.preventDefault();
      this.moreSheetOpen = false;
      return;
    }
    if (e.key !== "Tab") return;
    const focusable = this._getMoreSheetFocusable();
    if (focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (e.shiftKey) {
      if (document.activeElement === first) {
        e.preventDefault();
        last.focus();
      }
    } else if (document.activeElement === last) {
      e.preventDefault();
      first.focus();
    }
  }

  private _onHashChange(): void {
    const hash = window.location.hash.replace("#", "");
    if (!hash) return;
    const [tabPart, ...rest] = hash.split(":");
    const sessionPart = rest.join(":");
    const targetTab = tabPart as TabId;
    if (VALID_TABS.includes(targetTab)) {
      if (targetTab === "chat" && sessionPart) {
        this.chatSessionKey = sessionPart;
      }
      this._ensureLoaded(targetTab).then(() => {
        if (this._viewError) {
          this.tab = targetTab;
          return;
        }
        if (!document.startViewTransition) {
          this.tab = targetTab;
          return;
        }
        document.startViewTransition(() => {
          this.tab = targetTab;
          return this.updateComplete;
        });
      });
    } else {
      window.location.hash = "overview";
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
    if (e.key === "Escape" && this.moreSheetOpen) {
      e.preventDefault();
      this.moreSheetOpen = false;
    }
  }

  private _toggleSidebar(): void {
    this.sidebarCollapsed = !this.sidebarCollapsed;
    localStorage.setItem(SIDEBAR_KEY, String(this.sidebarCollapsed));
  }

  private _switchToDemo(): void {
    this._fallbackTimer = null;
    this._inFallbackWindow = false;
    // Tear down real gateway
    this.gateway?.removeEventListener("status", this._statusHandler);
    this.gateway?.disconnect();
    // Create and wire demo gateway
    const demo = new DemoGatewayClient() as unknown as GatewayClient;
    this.gateway = demo;
    setGateway(demo);
    demo.addEventListener("status", this._statusHandler);
    demo.connect("demo://fallback");
  }

  private async _ensureLoaded(tab: TabId): Promise<void> {
    if (loadedViews.has(tab)) return;
    try {
      await VIEW_IMPORTS[tab]();
      loadedViews.add(tab);
    } catch (e) {
      const err = e instanceof Error ? e : new Error(String(e));
      this._viewError = err;
    }
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
      <a
        href="#main-content"
        class="hu-skip-link"
        @click=${(e: Event) => {
          e.preventDefault();
          this.shadowRoot?.getElementById("main-content")?.focus();
        }}
        >Skip to content</a
      >
      ${this.connectionStatus === "disconnected" && !this._inFallbackWindow
        ? html`<div class="disconnect-banner" role="alert">
            Disconnected from server
            <button @click=${this._reconnect}>Reconnect</button>
          </div>`
        : nothing}
      <div class="layout ${this.sidebarCollapsed ? "collapsed" : ""}">
        <hu-sidebar
          .activeTab=${this.tab}
          ?collapsed=${this.sidebarCollapsed}
          .connectionStatus=${this.connectionStatus}
          @tab-change=${this._onTabChange}
          @toggle-collapse=${() => this._toggleSidebar()}
        ></hu-sidebar>

        <main id="main-content" tabindex="0">
          <div class="view-enter">
            <hu-error-boundary .error=${this._viewError} @retry=${this._onViewRetry}>
              ${this._renderWrappedView()}
            </hu-error-boundary>
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
            id="more-tab-btn"
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
            <div
              class="more-sheet"
              role="dialog"
              aria-modal="true"
              aria-label="More views"
              tabindex="-1"
            >
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

      <hu-command-palette
        .open=${this.commandPaletteOpen}
        @execute=${this._onCommandExecute}
        @close=${() => {
          this.commandPaletteOpen = false;
        }}
      ></hu-command-palette>

      <hu-shortcut-overlay
        .open=${this.shortcutOverlayOpen}
        @close=${() => {
          this.shortcutOverlayOpen = false;
        }}
      ></hu-shortcut-overlay>

      <hu-floating-mic></hu-floating-mic>
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
        return html`<hu-overview-view></hu-overview-view>`;
      case "chat":
        return html`<hu-chat-view .sessionKey=${this.chatSessionKey}></hu-chat-view>`;
      case "agents":
        return html`<hu-agents-view></hu-agents-view>`;
      case "sessions":
        return html`<hu-sessions-view></hu-sessions-view>`;
      case "models":
        return html`<hu-models-view></hu-models-view>`;
      case "config":
        return html`<hu-config-view></hu-config-view>`;
      case "tools":
        return html`<hu-tools-view></hu-tools-view>`;
      case "channels":
        return html`<hu-channels-view></hu-channels-view>`;
      case "automations":
        return html`<hu-automations-view></hu-automations-view>`;
      case "skills":
        return html`<hu-skills-view></hu-skills-view>`;
      case "voice":
        return html`<hu-voice-view></hu-voice-view>`;
      case "nodes":
        return html`<hu-nodes-view></hu-nodes-view>`;
      case "usage":
        return html`<hu-usage-view></hu-usage-view>`;
      case "metrics":
        return html`<hu-metrics-view></hu-metrics-view>`;
      case "memory":
        return html`<hu-memory-view></hu-memory-view>`;
      case "security":
        return html`<hu-security-view></hu-security-view>`;
      case "logs":
        return html`<hu-logs-view></hu-logs-view>`;
      default:
        return html`<hu-overview-view></hu-overview-view>`;
    }
  }
}
