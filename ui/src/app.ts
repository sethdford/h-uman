import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import type { GatewayClient, GatewayStatus } from "./gateway.js";
import { GatewayClient as GatewayClientClass } from "./gateway.js";
import { setGateway } from "./gateway-provider.js";
import { AUTH_FAILED } from "./gateway-aware.js";
import { icons } from "./icons.js";
import "./components/sidebar.js";
import "./components/hu-error-boundary.js";

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
  | "canvas"
  | "nodes"
  | "usage"
  | "metrics"
  | "memory"
  | "security"
  | "logs"
  | "turing"
  | "hula"
  | "workflow"
  | "design-system"
  | "settings";

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
  "canvas",
  "nodes",
  "usage",
  "metrics",
  "memory",
  "security",
  "logs",
  "turing",
  "hula",
  "workflow",
  "design-system",
  "settings",
];

const SIDEBAR_KEY = "hu-sidebar-collapsed";

/** Tabs that follow the list-detail archetype and render a `.detail-panel` at
 *  the wide breakpoint. Restored after b2ab0064 removed it for being "unused"
 *  on top of 7105152f which had stubbed `_hasDetail` to `return false`,
 *  silently disabling the dual-pane layout for sessions/channels/tools/nodes.
 *  Drives `<aside class="detail-panel">` rendering in `_renderLayout`. */
const LIST_DETAIL_TABS: TabId[] = ["sessions", "channels", "tools", "nodes", "canvas"];

const VIEW_IMPORTS: Record<TabId, () => Promise<unknown>> = {
  overview: () => import("./views/overview-view.js"),
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
  canvas: () => import("./views/canvas-view.js"),
  nodes: () => import("./views/nodes-view.js"),
  usage: () => import("./views/usage-view.js"),
  metrics: () => import("./views/metrics-view.js"),
  memory: () => import("./views/memory-view.js"),
  security: () => import("./views/security-view.js"),
  logs: () => import("./views/logs-view.js"),
  turing: () => import("./views/turing-view.js"),
  hula: () => import("./views/hula-view.js"),
  workflow: () => import("./views/workflow-view.js"),
  "design-system": () => import("./views/design-system-view.js"),
  settings: () => import("./views/settings-view.js"),
};

const loadedViews = new Set<TabId>();

const MOBILE_TABS: { id: TabId; label: string; icon: ReturnType<typeof html> }[] = [
  { id: "chat", label: "Chat", icon: icons["message-square"] },
  { id: "voice", label: "Voice", icon: icons.mic },
  { id: "agents", label: "Agents", icon: icons.zap },
  { id: "memory", label: "Memory", icon: icons.brain },
];

const MORE_TABS: { id: TabId; label: string; icon: ReturnType<typeof html> }[] = [
  { id: "skills", label: "Skills", icon: icons.grid },
  { id: "settings", label: "Settings", icon: icons.settings },
];

@customElement("hu-app")
export class ScApp extends LitElement {
  static override styles = css`
    @layer base, tokens, components, views, utilities;

    :host {
      display: block;
      height: 100vh;
      height: 100dvh;
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
      border: none;
      border-radius: var(--hu-radius-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: 600;
      cursor: pointer;
    }
    .hu-skip-link:focus-visible {
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

    /* Wide: detail panel for list-detail views */
    .detail-panel {
      display: none;
      background: var(--hu-bg-surface);
      border-left: 1px solid var(--hu-border);
      overflow: auto;
    }
    @media (min-width: 1240px) /* --hu-breakpoint-wide */ {
      .layout.has-detail {
        grid-template-columns: var(--hu-sidebar-width) 1fr var(--hu-detail-panel-width);
      }
      .layout.has-detail.collapsed {
        grid-template-columns: var(--hu-sidebar-collapsed) 1fr var(--hu-detail-panel-width);
      }
      .layout.has-detail .detail-panel {
        display: block;
      }
    }

    main {
      display: flex;
      flex-direction: column;
      overflow: hidden;
      min-height: 0;
      padding: var(--hu-space-lg);
      background: var(--hu-bg);
      view-transition-name: hu-main-content;
      position: relative;
      outline: none;
    }
    main.no-pad {
      padding: 0;
    }

    /* Keyboard-accessible scrollport (axe scrollable-region-focusable; nested views use shadow DOM). */
    .main-scroll {
      flex: 1;
      min-height: 0;
      min-width: 0;
      display: flex;
      flex-direction: column;
      overflow: auto;
      position: relative;
      outline: none;
      scrollbar-width: thin;
      scrollbar-color: var(--hu-border-subtle) transparent;
    }
    .main-scroll::-webkit-scrollbar {
      width: 6px;
    }
    .main-scroll::-webkit-scrollbar-track {
      background: transparent;
    }
    .main-scroll::-webkit-scrollbar-thumb {
      background: var(--hu-border-subtle);
      border-radius: var(--hu-radius-sm);
    }
    .main-scroll::-webkit-scrollbar-thumb:hover {
      background: var(--hu-border);
    }

    .scroll-progress {
      position: sticky;
      top: 0;
      z-index: 10;
      height: 2px;
      background: var(--hu-accent);
      transform-origin: left;
      flex-shrink: 0;
      opacity: 0;
      transition: opacity var(--hu-duration-normal, 200ms) var(--hu-ease-out);
    }
    main:has(:is([data-scrollable="true"])) .scroll-progress {
      opacity: 1;
    }
    @supports (animation-timeline: scroll()) {
      .scroll-progress {
        animation: hu-scroll-grow linear forwards;
        animation-timeline: scroll(nearest);
      }
    }
    @keyframes hu-scroll-grow {
      from {
        transform: scaleX(0);
      }
      to {
        transform: scaleX(1);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .scroll-progress {
        animation: none;
        transform: scaleX(1);
        opacity: 0;
      }
    }

    main:focus-visible,
    .main-scroll:focus-visible {
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

    /* Ambient lighting removed — clean background like claude.ai */

    .view-enter {
      flex: 1;
      min-height: 0;
      min-width: 0;
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

    .disconnect-banner,
    .demo-fallback-banner {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      z-index: 100;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-2xs) var(--hu-space-md);
      color: var(--hu-on-accent);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      animation: hu-slide-down var(--hu-duration-normal) var(--hu-ease-out);
    }
    .disconnect-banner {
      background: color-mix(in srgb, var(--hu-error) 85%, var(--hu-bg));
    }
    .demo-fallback-banner {
      background: color-mix(in srgb, var(--hu-accent-secondary) 85%, var(--hu-bg));
    }
    .banner-fading {
      animation: hu-fade-out var(--hu-duration-normal) var(--hu-ease-out) forwards;
    }
    @keyframes hu-slide-down {
      from {
        transform: translateY(-100%);
      }
      to {
        transform: translateY(0);
      }
    }
    .disconnect-banner button,
    .demo-fallback-banner button {
      background: var(--hu-bg);
      border: 1px solid var(--hu-on-accent);
      color: var(--hu-text);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-sm);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
      cursor: pointer;
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
      line-height: 1;
    }
    .disconnect-banner button:hover,
    .demo-fallback-banner button:hover {
      background: var(--hu-bg-surface);
    }
    .banner-dismiss {
      background: transparent;
      border: none;
      color: var(--hu-on-accent);
      cursor: pointer;
      padding: var(--hu-space-2xs);
      font-size: var(--hu-text-xs);
      line-height: 1;
      opacity: 0.8;
      margin-inline-start: var(--hu-space-xs);
    }
    .banner-dismiss:hover {
      opacity: 1;
    }

    .mobile-nav {
      display: none;
    }

    /*
     * Shell layout breakpoints (599 / 904 / 1240px) are intentional for sidebar +
     * mobile nav; they differ slightly from token ladder (480 / 640 / 768 / 1024)
     * used in views via @container. Keep both — do not merge without retesting grid.
     */
    /* Compact (<600px): mobile bottom nav, single column */
    @media (max-width: 599px) /* --hu-breakpoint-compact */ {
      .layout {
        grid-template-columns: 1fr;
        grid-template-rows: 1fr auto;
      }
      .layout.collapsed,
      .layout.has-detail {
        grid-template-columns: 1fr;
        grid-template-rows: 1fr auto;
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
        backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
          saturate(var(--hu-glass-subtle-saturate, 120%));
        -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
          saturate(var(--hu-glass-subtle-saturate, 120%));
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
        transition: color var(--hu-duration-fast) var(--hu-ease-out);
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

    /* Medium (600–904px): collapsed sidebar, 2-column content */
    @media (min-width: 600px) and (max-width: 904px) /* --hu-breakpoint-compact to --hu-breakpoint-medium */ {
      .layout {
        grid-template-columns: var(--hu-sidebar-collapsed) 1fr;
      }
      .layout.collapsed {
        grid-template-columns: var(--hu-sidebar-collapsed) 1fr;
      }
      .layout.has-detail {
        grid-template-columns: var(--hu-sidebar-collapsed) 1fr;
      }
      hu-sidebar {
        display: flex;
      }
      .mobile-nav {
        display: none;
      }
      main {
        padding: var(--hu-space-lg);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .layout {
        transition: none;
      }
    }

    .more-backdrop {
      position: fixed;
      inset: 0;
      z-index: 9998;
      background: color-mix(in srgb, var(--hu-bg) 40%, transparent);
      backdrop-filter: blur(var(--hu-blur-sm, 4px));
      -webkit-backdrop-filter: blur(var(--hu-blur-sm, 4px));
      animation: hu-backdrop-dim var(--hu-duration-normal) var(--hu-ease-out) both;
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
      animation: hu-sheet-up var(--hu-duration-normal) var(--hu-ease-spring) both;
      max-height: 70vh;
      max-height: 70dvh;
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
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
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

  @state() private tab: TabId = "chat";
  @state() private chatSessionKey = "default";
  @state() private connectionStatus: GatewayStatus = "disconnected";
  @state() private sidebarCollapsed = false;
  @state() private _isMediumViewport = false;
  @state() private commandPaletteOpen = false;
  @state() private shortcutOverlayOpen = false;
  @state() private moreSheetOpen = false;
  @state() private _viewError: Error | null = null;
  @state() private _inFallbackWindow = false;
  @state() private _demoFallback = false;
  @state() private _bannerDismissed = false;
  private _bannerAutoTimer: ReturnType<typeof setTimeout> | null = null;

  gateway: GatewayClient | null = null;
  private _keyHandler = this._onGlobalKey.bind(this);
  private _paletteEscapeCapture = ((e: KeyboardEvent) => {
    if (e.key === "Escape" && this.commandPaletteOpen) {
      e.stopPropagation();
      this.commandPaletteOpen = false;
    }
  }).bind(this) as EventListener;
  private _hashHandler = this._onHashChange.bind(this);
  private _resizeHandler = this._onResize.bind(this);
  private _pendingGKey = false;
  private _pendingGTimeout: ReturnType<typeof setTimeout> | null = null;
  private _moreSheetKeyHandler = this._onMoreSheetKeyDown.bind(this);
  private _moreSheetPreviousElement: HTMLElement | null = null;
  private readonly _moreSheetFocusableSelector =
    'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
  private _statusHandler = ((e: CustomEvent<GatewayStatus>) => {
    this.connectionStatus = e.detail;
  }) as EventListener;
  private _fallbackTimer: ReturnType<typeof setTimeout> | null = null;
  private _navHoverTimer: ReturnType<typeof setTimeout> | null = null;
  private _authFailedHandler = (() => {
    if (!this._isDemo) this._switchToDemo();
  }) as EventListener;

  private _onNavigate = ((e: Event) => {
    const raw = (e as CustomEvent<string>).detail as string;
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
  }) as EventListener;

  private _onVoiceTranscribe = ((e: Event) => {
    const ce = e as CustomEvent<{ audio: string; mimeType: string }>;
    if (!this.gateway) return;
    this.gateway
      .request<{ text?: string }>("voice.transcribe", {
        audio: ce.detail.audio,
        mimeType: ce.detail.mimeType,
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
  }) as EventListener;

  private _onGatewayFeatures = (() => {
    if (this._fallbackTimer) {
      clearTimeout(this._fallbackTimer);
      this._fallbackTimer = null;
      this._inFallbackWindow = false;
    }
  }) as EventListener;

  private get _isDemo(): boolean {
    return new URLSearchParams(window.location.search).has("demo");
  }

  private get _hasDetail(): boolean {
    return LIST_DETAIL_TABS.includes(this.tab);
  }

  private get prefersReducedMotion(): boolean {
    return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  }

  private get _effectiveSidebarCollapsed(): boolean {
    return this.sidebarCollapsed || this._isMediumViewport;
  }

  private get _isConversationalView(): boolean {
    return this.tab === "chat" || this.tab === "voice";
  }

  override connectedCallback(): void {
    super.connectedCallback();

    // First-time visitors get an EXPANDED sidebar so navigation is discoverable.
    // The `?: true` default introduced in b7abafcc collapsed the sidebar by
    // default, which broke `sidebar-transitions.spec.ts::sidebar starts expanded
    // with correct width`. Only honor the stored value when it exists.
    const storedSidebar = localStorage.getItem(SIDEBAR_KEY);
    this.sidebarCollapsed = storedSidebar === "true";
    this._bannerDismissed = sessionStorage.getItem("hu-banner-dismissed") === "true";
    this._updateViewportBreakpoint();
    window.addEventListener("resize", this._resizeHandler);
    document.addEventListener("keydown", this._paletteEscapeCapture, true);
    document.addEventListener("keydown", this._keyHandler);
    document.addEventListener(AUTH_FAILED, this._authFailedHandler);
    window.addEventListener("hashchange", this._hashHandler);
    this._onHashChange();
    import("./lib/dynamic-light.js").then((m) => m.dynamicLight.start());
    import("./lib/ambient-intelligence.js").then((m) => m.ambientIntelligence.start(this));

    void this._initGateway();

    this.addEventListener("navigate", this._onNavigate);
    this.addEventListener("hu-voice-transcribe", this._onVoiceTranscribe);
  }

  override updated(changedProperties: PropertyValues): void {
    if (changedProperties.has("_demoFallback") && this._demoFallback && !this._bannerDismissed) {
      this._bannerAutoTimer = setTimeout(() => {
        this._dismissBanner();
      }, 5000);
    }
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

  override firstUpdated(): void {
    const idle = (fn: () => void) =>
      "requestIdleCallback" in window ? requestIdleCallback(fn) : setTimeout(fn, 0);
    idle(() => {
      this._prefetchOnIdle();
      import("./components/floating-mic.js");
      import("./components/command-palette.js");
      import("./components/hu-shortcut-overlay.js");
    });
  }

  override disconnectedCallback(): void {
    this.removeEventListener("navigate", this._onNavigate);
    this.removeEventListener("hu-voice-transcribe", this._onVoiceTranscribe);
    this._clearPendingG();
    if (this._navHoverTimer) {
      clearTimeout(this._navHoverTimer);
      this._navHoverTimer = null;
    }
    document.removeEventListener("keydown", this._moreSheetKeyHandler);
    document.removeEventListener("keydown", this._paletteEscapeCapture, true);
    document.removeEventListener("keydown", this._keyHandler);
    window.removeEventListener("resize", this._resizeHandler);
    document.removeEventListener(AUTH_FAILED, this._authFailedHandler);
    window.removeEventListener("hashchange", this._hashHandler);
    import("./lib/dynamic-light.js").then((m) => m.dynamicLight.stop());
    import("./lib/ambient-intelligence.js").then((m) => m.ambientIntelligence.stop());
    if (this._fallbackTimer) {
      clearTimeout(this._fallbackTimer);
      this._fallbackTimer = null;
    }
    if (this._bannerAutoTimer) {
      clearTimeout(this._bannerAutoTimer);
      this._bannerAutoTimer = null;
    }
    this.gateway?.removeEventListener("status", this._statusHandler);
    this.gateway?.removeEventListener("features", this._onGatewayFeatures);
    this.gateway?.disconnect();
    super.disconnectedCallback();
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
    if (!hash) {
      void this._ensureLoaded("chat");
      return;
    }
    const [tabPart, ...rest] = hash.split(":");
    const sessionPart = rest.join(":");
    const targetTab = tabPart as TabId;
    if (VALID_TABS.includes(targetTab)) {
      if (targetTab === "chat" && sessionPart) {
        this.chatSessionKey = sessionPart;
      }
      void this._applyHashRoute(targetTab);
    } else {
      window.location.hash = "chat";
    }
  }

  private async _applyHashRoute(targetTab: TabId): Promise<void> {
    await this._ensureLoaded(targetTab);
    if (this._viewError) {
      this.tab = targetTab;
      return;
    }
    await this._switchView(targetTab);
  }

  private _onGlobalKey(e: KeyboardEvent): void {
    const mod = e.metaKey || e.ctrlKey;
    const modShift = mod && e.shiftKey;
    if (mod && e.key === "k") {
      e.preventDefault();
      this.commandPaletteOpen = !this.commandPaletteOpen;
      document.dispatchEvent(new CustomEvent("focus-search"));
    }
    if (mod && e.key === "b") {
      e.preventDefault();
      this._toggleSidebar();
    }
    if (modShift && e.key === "N") {
      e.preventDefault();
      document.dispatchEvent(new CustomEvent("new-chat"));
    }
    if (modShift && e.key === "S") {
      e.preventDefault();
      document.dispatchEvent(new CustomEvent("toggle-sessions"));
    }
    if (modShift && e.key === "A") {
      e.preventDefault();
      document.dispatchEvent(new CustomEvent("toggle-artifacts"));
    }
    if (modShift && e.key === "T") {
      e.preventDefault();
      this._dispatchToggleTheme();
    }
    if (modShift && e.key === "E") {
      e.preventDefault();
      this._dispatchExportLogs();
    }
    if (e.key === "?" && !this._isInputFocused()) {
      e.preventDefault();
      this.shortcutOverlayOpen = !this.shortcutOverlayOpen;
      document.dispatchEvent(new CustomEvent("show-shortcuts"));
    }
    if (e.key === "Escape") {
      if (this.moreSheetOpen) {
        e.preventDefault();
        this.moreSheetOpen = false;
      }
      if (this._pendingGKey) {
        this._clearPendingG();
      }
      document.dispatchEvent(new CustomEvent("close-panel"));
    }
    if (!this._isInputFocused()) {
      this._handleVimKeys(e);
    }
  }

  private _isInputFocused(): boolean {
    const el = document.activeElement;
    if (!el) return false;
    const tag = el.tagName?.toLowerCase();
    const role = (el as HTMLElement).getAttribute?.("role");
    if (tag === "input" || tag === "textarea" || tag === "select") return true;
    if (role === "textbox" || (el as HTMLElement).isContentEditable) return true;
    return false;
  }

  private _handleVimKeys(e: KeyboardEvent): void {
    if (this.commandPaletteOpen || this.shortcutOverlayOpen || this.moreSheetOpen) return;
    if (e.key === "/") {
      e.preventDefault();
      this.commandPaletteOpen = true;
      return;
    }
    if (e.key === "j" || e.key === "k") {
      const sidebar = this.shadowRoot?.querySelector("hu-sidebar") as {
        focusNextNavItem?: () => void;
        focusPrevNavItem?: () => void;
      } | null;
      if (sidebar?.focusNextNavItem && sidebar?.focusPrevNavItem) {
        e.preventDefault();
        if (e.key === "j") sidebar.focusNextNavItem();
        else sidebar.focusPrevNavItem();
      }
      return;
    }
    if (e.key === "g") {
      if (this._pendingGKey) {
        this._clearPendingG();
        return;
      }
      e.preventDefault();
      this._pendingGKey = true;
      this._pendingGTimeout = setTimeout(() => this._clearPendingG(), 1000);
      return;
    }
    if (this._pendingGKey) {
      e.preventDefault();
      this._clearPendingG();
      const map: Record<string, TabId> = {
        c: "chat",
        v: "voice",
        p: "canvas",
        e: "memory",
        a: "agents",
        s: "settings",
      };
      const tab = map[e.key.toLowerCase()];
      if (tab && VALID_TABS.includes(tab)) {
        this._switchTab(tab);
      }
    }
  }

  private _clearPendingG(): void {
    this._pendingGKey = false;
    if (this._pendingGTimeout) {
      clearTimeout(this._pendingGTimeout);
      this._pendingGTimeout = null;
    }
  }

  private _dispatchToggleTheme(): void {
    const sidebar = this.shadowRoot?.querySelector("hu-sidebar") as {
      _cycleTheme?: () => void;
    } | null;
    sidebar?._cycleTheme?.();
  }

  private _dispatchExportLogs(): void {
    this._switchTab("logs").then(() => {
      this.updateComplete.then(() => {
        const logsView = this.shadowRoot?.querySelector("hu-logs-view") as {
          exportLogs?: () => void;
        } | null;
        logsView?.exportLogs?.();
      });
    });
  }

  private _onResize(): void {
    this._updateViewportBreakpoint();
  }

  private _updateViewportBreakpoint(): void {
    const w = window.innerWidth;
    const was = this._isMediumViewport;
    const now = w >= 600 && w <= 904;
    if (was !== now) {
      this._isMediumViewport = now;
    }
  }

  private _toggleSidebar(): void {
    this.sidebarCollapsed = !this.sidebarCollapsed;
    localStorage.setItem(SIDEBAR_KEY, String(this.sidebarCollapsed));
  }

  private async _createDemoGateway(): Promise<GatewayClient> {
    const { DemoGatewayClient } = await import("./demo-gateway.js");
    return new DemoGatewayClient() as unknown as GatewayClient;
  }

  private async _initGateway(): Promise<void> {
    const wsUrl =
      typeof window !== "undefined" &&
      (window as unknown as { __VITE_WS_PROXY__?: string }).__VITE_WS_PROXY__
        ? (window as unknown as { __VITE_WS_PROXY__: string }).__VITE_WS_PROXY__
        : (() => {
            const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
            return `${proto}//${window.location.host}/ws`;
          })();

    if (this._isDemo) {
      const demo = await this._createDemoGateway();
      this.gateway = demo;
      setGateway(demo);
      demo.addEventListener("status", this._statusHandler);
      demo.connect(wsUrl);
      return;
    }

    const gw = new GatewayClientClass();
    this.gateway = gw;
    setGateway(gw);
    gw.addEventListener("status", this._statusHandler);

    this._inFallbackWindow = true;
    this._fallbackTimer = setTimeout(() => {
      this._switchToDemo();
    }, 5000);
    gw.addEventListener("features", this._onGatewayFeatures);

    gw.connect(wsUrl);
  }

  private _switchToDemo(): void {
    this._fallbackTimer = null;
    this._inFallbackWindow = false;
    this._demoFallback = true;
    this.gateway?.removeEventListener("status", this._statusHandler);
    this.gateway?.removeEventListener("features", this._onGatewayFeatures);
    // Don't disconnect the real gateway — let it keep reconnecting in background
    // so it can recover if the server comes up later
    this._createDemoGateway()
      .then((demo) => {
        if (!this.isConnected) return;
        this.gateway = demo;
        setGateway(demo);
        demo.addEventListener("status", this._statusHandler);
        demo.connect("demo://fallback");
      })
      .catch(() => {
        if (!this.isConnected) return;
      });
  }

  private async _ensureLoaded(tab: TabId): Promise<void> {
    if (loadedViews.has(tab)) return;
    try {
      await VIEW_IMPORTS[tab]();
      loadedViews.add(tab);
      this._viewError = null;
    } catch (e) {
      const err = e instanceof Error ? e : new Error(String(e));
      this._viewError = err;
    }
  }

  private _prefetchSilent(tab: TabId): void {
    if (loadedViews.has(tab)) return;
    this._ensureLoaded(tab).catch(() => {});
  }

  private _prefetchOnIdle(): void {
    const targets: TabId[] = ["chat", "voice", "agents"];
    for (const t of targets) this._prefetchSilent(t);
  }

  private _handleNavHover(e: CustomEvent<string>): void {
    const tab = e.detail as TabId;
    if (!VALID_TABS.includes(tab)) return;
    if (this._navHoverTimer) clearTimeout(this._navHoverTimer);
    this._navHoverTimer = setTimeout(() => {
      this._navHoverTimer = null;
      this._prefetchSilent(tab);
    }, 200);
  }

  private _prefetchAdjacent(tab: TabId): void {
    const idx = VALID_TABS.indexOf(tab);
    if (idx < 0) return;
    if (idx > 0) this._prefetchSilent(VALID_TABS[idx - 1]!);
    if (idx < VALID_TABS.length - 1) this._prefetchSilent(VALID_TABS[idx + 1]!);
  }

  private _performViewSwitch(newTab: TabId): void {
    this.tab = newTab;
  }

  private async _switchView(newTab: TabId): Promise<void> {
    if (document.startViewTransition && !this.prefersReducedMotion) {
      try {
        const transition = document.startViewTransition(() => {
          this._performViewSwitch(newTab);
          return this.updateComplete;
        });
        await transition.finished;
      } catch {
        this._performViewSwitch(newTab);
        await this.updateComplete;
      }
    } else {
      this._performViewSwitch(newTab);
      await this.updateComplete;
    }
  }

  private async _switchTab(tab: TabId): Promise<void> {
    if (this.tab === tab) return;
    await this._ensureLoaded(tab);
    const hash = tab === "chat" ? `#chat:${this.chatSessionKey}` : `#${tab}`;
    window.history.replaceState(null, "", hash);
    await this._switchView(tab);
    this._prefetchAdjacent(tab);
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
    } else if (action === "toggle-theme") {
      this._dispatchToggleTheme();
    } else if (action === "export-logs") {
      this._dispatchExportLogs();
    } else if (action === "refresh") {
      const view = this.shadowRoot?.querySelector("main")?.firstElementChild;
      if (view && "load" in view && typeof (view as { load: () => void }).load === "function") {
        (view as { load: () => void }).load();
      }
    }
  }

  override render() {
    return html`
      <button
        class="hu-skip-link"
        @click=${() => {
          this.shadowRoot?.getElementById("main-scroll")?.focus();
        }}
      >
        Skip to content
      </button>
      ${this.connectionStatus === "disconnected" &&
      !this._inFallbackWindow &&
      !this._bannerDismissed
        ? html`<div class="disconnect-banner" role="alert">
            Disconnected from server
            <button @click=${this._reconnect}>Reconnect</button>
            <button class="banner-dismiss" @click=${this._dismissBanner} aria-label="Dismiss">
              ×
            </button>
          </div>`
        : nothing}
      ${this._demoFallback && !this._bannerDismissed
        ? html`<div class="demo-fallback-banner" role="status">
            Demo mode — gateway not reachable
            <button @click=${this._reconnect}>Retry</button>
            <button class="banner-dismiss" @click=${this._dismissBanner} aria-label="Dismiss">
              ×
            </button>
          </div>`
        : nothing}
      <div
        class="layout ${this._effectiveSidebarCollapsed ? "collapsed" : ""} ${this._hasDetail
          ? "has-detail"
          : ""}"
      >
        <hu-sidebar
          class="hu-glass-scroll-aware"
          .activeTab=${this.tab}
          ?collapsed=${this._effectiveSidebarCollapsed}
          .connectionStatus=${this.connectionStatus}
          @tab-change=${this._onTabChange}
          @nav-hover=${this._handleNavHover}
          @toggle-collapse=${() => this._toggleSidebar()}
        ></hu-sidebar>

        <main
          id="main-content"
          tabindex="-1"
          role="main"
          class="${this._isConversationalView ? "no-pad" : ""}"
        >
          <div
            id="main-scroll"
            class="main-scroll view-content"
            tabindex="0"
            role="region"
            aria-label="Main content"
          >
            <div class="scroll-progress" role="presentation" aria-hidden="true"></div>
            <div class="view-enter">
              <hu-error-boundary .error=${this._viewError} @retry=${this._onViewRetry}>
                ${this._renderWrappedView()}
              </hu-error-boundary>
            </div>
          </div>
        </main>

        ${this._hasDetail
          ? html`<aside class="detail-panel" role="complementary" aria-label="Detail"></aside>`
          : nothing}

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

      ${nothing}
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

  private _dismissBanner(): void {
    this._bannerDismissed = true;
    sessionStorage.setItem("hu-banner-dismissed", "true");
    if (this._bannerAutoTimer) {
      clearTimeout(this._bannerAutoTimer);
      this._bannerAutoTimer = null;
    }
  }

  private _reconnect(): void {
    this._demoFallback = false;
    this.gateway?.removeEventListener("status", this._statusHandler);
    this.gateway?.removeEventListener("features", this._onGatewayFeatures);
    this.gateway?.disconnect();
    this._initGateway();
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
      case "canvas":
        return html`<hu-canvas-view></hu-canvas-view>`;
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
      case "turing":
        return html`<hu-turing-view></hu-turing-view>`;
      case "hula":
        return html`<hu-hula-view></hu-hula-view>`;
      case "workflow":
        return html`<hu-workflow-view></hu-workflow-view>`;
      case "design-system":
        return html`<hu-design-system-view></hu-design-system-view>`;
      case "settings":
        return html`<hu-settings-view></hu-settings-view>`;
      default:
        return html`<hu-chat-view .sessionKey=${this.chatSessionKey}></hu-chat-view>`;
    }
  }
}
