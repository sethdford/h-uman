import { LitElement, html, css, nothing } from "lit";
import type { PropertyValues } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import brandLogoSvg from "../assets/logo.svg?raw";
import { icons } from "../icons.js";
import "./hu-status-dot.js";

type ConnectionStatus = "connected" | "connecting" | "disconnected";

interface NavItem {
  id: string;
  label: string;
  icon: ReturnType<typeof html>;
}

const NAV_SHORTCUT_MAP: Record<string, string> = {
  chat: "g c",
  voice: "g v",
  canvas: "g p",
  memory: "g e",
  agents: "g a",
  settings: "g s",
};

const PRIMARY_NAV: NavItem[] = [
  { id: "chat", label: "Chat", icon: icons["message-square"] },
  { id: "voice", label: "Voice", icon: icons.mic },
];

const SECONDARY_NAV: NavItem[] = [
  { id: "agents", label: "Agents", icon: icons.zap },
  { id: "memory", label: "Memory", icon: icons.brain },
  { id: "skills", label: "Skills", icon: icons.grid },
];

const SYSTEM_NAV: NavItem[] = [
  { id: "canvas", label: "Canvas", icon: icons.monitor },
  { id: "nodes", label: "Nodes", icon: icons.cpu },
  { id: "metrics", label: "Metrics", icon: icons.activity },
  { id: "logs", label: "Logs", icon: icons["file-text"] },
  { id: "security", label: "Security", icon: icons.lock },
  { id: "automations", label: "Automations", icon: icons.clock },
];

const SETTINGS_NAV: NavItem = {
  id: "settings",
  label: "Settings",
  icon: icons.settings,
};

/** Pointer-proximity pull for expanded sidebar nav (see magnetic listeners). */
const HU_SIDEBAR_MAGNETIC_RADIUS_PX = 200;
const HU_SIDEBAR_MAGNETIC_MAX_SHIFT_PX = 3;
const HU_SIDEBAR_MAGNETIC_MAX_ITEMS = 20;

@customElement("hu-sidebar")
export class ScSidebar extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      width: var(--hu-sidebar-width);
      min-width: var(--hu-sidebar-width);
      contain: layout style;
      z-index: 1;
      /* Glass-standard equivalent — uses tokens directly because :host
         can't receive utility classes from theme.css light DOM */
      background: color-mix(
        in srgb,
        var(--hu-bg-surface) var(--hu-glass-standard-bg-opacity, 8%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 120%));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 120%));
      border-right: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 10%),
          transparent
        );
      transition:
        width var(--hu-duration-normal) var(--hu-spring-out, cubic-bezier(0.34, 1.56, 0.64, 1)),
        min-width var(--hu-duration-normal) var(--hu-spring-out, cubic-bezier(0.34, 1.56, 0.64, 1));
      will-change: width;
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

    .sidebar {
      display: flex;
      flex-direction: column;
      height: 100%;
      min-height: 0;
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
      view-transition-name: hu-sidebar-logo;
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
      padding: var(--hu-space-md);
    }

    .nav-section {
      margin-bottom: var(--hu-space-sm);
    }

    .nav-divider {
      height: 1px;
      background: color-mix(in srgb, var(--hu-border) 50%, transparent);
      margin: var(--hu-space-sm) var(--hu-space-sm);
    }

    .nav-group-label {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      width: 100%;
      padding: var(--hu-space-xs) var(--hu-space-md);
      background: none;
      border: none;
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text-tertiary);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      cursor: pointer;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .nav-group-label:hover {
      color: var(--hu-text-secondary);
    }

    .nav-group-label .chevron {
      width: var(--hu-icon-xs, 0.75rem);
      height: var(--hu-icon-xs, 0.75rem);
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .nav-group-label .chevron.open {
      transform: rotate(90deg);
    }

    :host([collapsed]) .nav-group-label {
      justify-content: center;
    }

    :host([collapsed]) .nav-group-label span:not(.chevron) {
      display: none;
    }

    .system-items {
      overflow: hidden;
      max-height: 0;
      opacity: 0;
      visibility: hidden;
      transition:
        max-height var(--hu-duration-normal) var(--hu-ease-out),
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        visibility 0s var(--hu-duration-normal);
    }

    .system-items.open {
      max-height: 25rem;
      opacity: 1;
      visibility: visible;
      transition:
        max-height var(--hu-duration-normal) var(--hu-ease-out),
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        visibility 0s;
    }

    .settings-item {
      border-left: 1px solid transparent;
      margin-bottom: var(--hu-space-xs);
    }

    .nav-item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
      width: 100%;
      min-height: var(--hu-button-min-height);
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: transparent;
      border: none;
      border-left: 2px solid transparent;
      border-radius: var(--hu-radius-sm);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
      cursor: pointer;
      transition:
        background-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
      margin-bottom: var(--hu-space-xs);
      text-align: left;
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
      animation: hu-nav-enter var(--hu-duration-normal, 300ms) var(--hu-ease-out) both;
    }
    @keyframes hu-nav-enter {
      from {
        opacity: 0;
        transform: translateX(calc(-1 * var(--hu-space-sm)));
      }
      to {
        opacity: 1;
        transform: translateX(0);
      }
    }
    .nav-section .nav-item:nth-child(1) {
      animation-delay: 50ms;
    }
    .nav-section .nav-item:nth-child(2) {
      animation-delay: 100ms;
    }
    .nav-section .nav-item:nth-child(3) {
      animation-delay: 150ms;
    }
    .nav-section .nav-item:nth-child(4) {
      animation-delay: 200ms;
    }
    .nav-section .nav-item:nth-child(5) {
      animation-delay: 250ms;
    }

    .nav-item {
      &:active {
        transform: scale(0.97);
      }

      &:focus-visible {
        outline: var(--hu-focus-ring-width) solid var(--hu-accent);
        outline-offset: calc(-1 * var(--hu-focus-ring-width));
        border-radius: var(--hu-radius);
      }

      &:hover:not([aria-current]) {
        background: var(--hu-hover-overlay);
        color: var(--hu-text);
      }

      &[aria-current] {
        background: var(--hu-surface-container-high);
        border-left: 2px solid var(--hu-accent);
        border-radius: var(--hu-radius-sm) 0 0 var(--hu-radius-sm);
        color: var(--hu-text);
        font-weight: var(--hu-weight-semibold);
      }
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
      color: var(--hu-text);
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
      min-height: var(--hu-button-min-height);
      padding: var(--hu-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text);
      cursor: pointer;
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .theme-toggle:hover {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    .theme-toggle:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: calc(-1 * var(--hu-focus-ring-width));
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
      min-height: var(--hu-button-min-height);
      padding: var(--hu-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-normal) var(--hu-ease-out);
    }

    .collapse-btn:active {
      transform: scale(0.97);
    }

    .collapse-btn:hover {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    .collapse-btn:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: calc(-1 * var(--hu-focus-ring-width));
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
      margin-inline-start: var(--hu-space-sm);
    }

    .nav-item-wrap {
      position: relative;
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .nav.nav--magnetic-tracking .nav-item-wrap {
      transition: none;
    }

    @media (prefers-reduced-motion: reduce) {
      .nav-item-wrap {
        transition: none;
      }
    }

    .shortcut-tooltip {
      position: absolute;
      left: 100%;
      top: 50%;
      transform: translateY(-50%);
      margin-inline-start: var(--hu-space-sm);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font-mono);
      color: var(--hu-text-muted);
      background: var(--hu-surface-container-highest);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      box-shadow: var(--hu-shadow-md);
      white-space: nowrap;
      z-index: 100;
      pointer-events: none;
      animation: hu-fade-in var(--hu-duration-fast) var(--hu-ease-out);
    }

    :host([collapsed]) .shortcut-tooltip {
      display: none;
    }

    @media (prefers-reduced-motion: reduce) {
      .shortcut-tooltip {
        animation: none;
      }
    }
  `;

  @property({ type: String }) activeTab = "chat";
  @property({ type: Boolean, reflect: true }) collapsed = false;
  @property({ type: String }) connectionStatus: ConnectionStatus = "disconnected";
  @property({ type: String }) theme: "system" | "dark" | "light" = (() => {
    try {
      return (localStorage.getItem("hu-theme") as "system" | "dark" | "light") || "system";
    } catch {
      return "system";
    }
  })();

  @state() private _hoveredItemId: string | null = null;
  @state() private _showShortcutTooltip = false;
  @state() private _systemOpen = false;
  private _hoverTimer: ReturnType<typeof setTimeout> | null = null;

  private _mqlHoverNone: MediaQueryList | null = null;
  private _mqlReduceMotion: MediaQueryList | null = null;
  private _magneticNav: HTMLElement | null = null;
  private _magneticRafId: number | null = null;
  private _magneticPointer: { clientX: number; clientY: number } | null = null;

  private readonly _onMagneticMql = (): void => {
    this._syncMagneticNavListeners();
  };

  private readonly _onMagneticPointerMove = (e: PointerEvent): void => {
    if (!this._magneticInteractionAllowed() || !this._magneticNav) return;
    this._magneticPointer = { clientX: e.clientX, clientY: e.clientY };
    this._magneticNav.classList.add("nav--magnetic-tracking");
    if (this._magneticRafId === null) {
      this._magneticRafId = requestAnimationFrame(() => this._flushMagneticFrame());
    }
  };

  private readonly _onMagneticPointerEnd = (): void => {
    if (this._magneticRafId !== null) {
      cancelAnimationFrame(this._magneticRafId);
      this._magneticRafId = null;
    }
    this._magneticPointer = null;
    const nav = this._magneticNav;
    if (nav) {
      nav.classList.remove("nav--magnetic-tracking");
      this._resetMagneticNavItemWraps(nav);
    }
  };

  private _renderNavItem(item: NavItem) {
    return html`
      <div
        class="nav-item-wrap"
        @mouseenter=${() => this._onNavItemEnter(item.id)}
        @mouseleave=${() => this._onNavItemLeave()}
      >
        <button
          class="nav-item"
          data-nav-id=${item.id}
          ?aria-current=${this.activeTab === item.id}
          aria-label=${item.label}
          aria-describedby=${this._showShortcutTooltip && this._hoveredItemId === item.id
            ? `shortcut-${item.id}`
            : undefined}
          title=${this.collapsed ? item.label : undefined}
          @click=${() => this._dispatchTabChange(item.id)}
          @mouseenter=${() => this._dispatchNavHover(item.id)}
        >
          <span class="icon">${item.icon}</span>
          <span class="label">${item.label}</span>
        </button>
        ${this._showShortcutTooltip && this._hoveredItemId === item.id
          ? html`<span class="shortcut-tooltip" id=${`shortcut-${item.id}`} role="tooltip"
              >${NAV_SHORTCUT_MAP[item.id] ?? ""}</span
            >`
          : nothing}
      </div>
    `;
  }

  override render() {
    return html`
      <aside class="sidebar">
        <header class="header" role="banner">
          <div class="logo" aria-hidden="true">${unsafeHTML(brandLogoSvg)}</div>
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

        <nav class="nav" aria-label="Main navigation" @keydown=${this._onNavKeyDown}>
          <div class="nav-section">${PRIMARY_NAV.map((item) => this._renderNavItem(item))}</div>
          <div class="nav-divider"></div>
          <div class="nav-section">${SECONDARY_NAV.map((item) => this._renderNavItem(item))}</div>
          <div class="nav-divider"></div>
          <div class="nav-section">
            <button
              class="nav-group-label"
              @click=${() => {
                this._systemOpen = !this._systemOpen;
              }}
              aria-expanded=${this._systemOpen}
            >
              <span class="chevron ${this._systemOpen ? "open" : ""}">${icons.chevron}</span>
              <span>System</span>
            </button>
            <div class="system-items ${this._systemOpen ? "open" : ""}">
              ${SYSTEM_NAV.map((item) => this._renderNavItem(item))}
            </div>
          </div>
        </nav>

        <footer class="footer">
          <button
            class="nav-item settings-item"
            data-nav-id=${SETTINGS_NAV.id}
            ?aria-current=${this.activeTab === SETTINGS_NAV.id}
            aria-label=${SETTINGS_NAV.label}
            title=${this.collapsed ? SETTINGS_NAV.label : undefined}
            @click=${() => this._dispatchTabChange(SETTINGS_NAV.id)}
            @mouseenter=${() => this._dispatchNavHover(SETTINGS_NAV.id)}
          >
            <span class="icon">${SETTINGS_NAV.icon}</span>
            <span class="label">${SETTINGS_NAV.label}</span>
          </button>
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
    const apply = () => {
      if (this.theme === "system") {
        root.removeAttribute("data-theme");
      } else {
        root.setAttribute("data-theme", this.theme);
      }
    };

    if (
      typeof document.startViewTransition === "function" &&
      !window.matchMedia("(prefers-reduced-motion: reduce)").matches
    ) {
      document.startViewTransition(apply);
    } else {
      apply();
    }
  }

  override connectedCallback() {
    super.connectedCallback();
    this._applyTheme();
    this._mqlHoverNone = window.matchMedia("(hover: none)");
    this._mqlReduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
    this._mqlHoverNone.addEventListener("change", this._onMagneticMql);
    this._mqlReduceMotion.addEventListener("change", this._onMagneticMql);
  }

  override firstUpdated(changed: PropertyValues): void {
    super.firstUpdated(changed);
    this._syncMagneticNavListeners();
  }

  override updated(changed: PropertyValues): void {
    super.updated(changed);
    if (changed.has("collapsed")) {
      this._syncMagneticNavListeners();
    }
    if (changed.has("activeTab")) {
      const systemIds = SYSTEM_NAV.map((n) => n.id);
      if (systemIds.includes(this.activeTab)) {
        this._systemOpen = true;
      }
    }
  }

  private _onNavItemEnter(itemId: string): void {
    if (this._hoverTimer) clearTimeout(this._hoverTimer);
    this._hoverTimer = setTimeout(() => {
      this._hoverTimer = null;
      this._hoveredItemId = itemId;
      this._showShortcutTooltip = !!NAV_SHORTCUT_MAP[itemId];
    }, 1000);
  }

  private _onNavItemLeave(): void {
    if (this._hoverTimer) {
      clearTimeout(this._hoverTimer);
      this._hoverTimer = null;
    }
    this._hoveredItemId = null;
    this._showShortcutTooltip = false;
  }

  focusNextNavItem(): void {
    const items = this._getNavItems();
    if (items.length === 0) return;
    const idx = items.findIndex((el) => el === document.activeElement);
    const next = idx < 0 ? 0 : (idx + 1) % items.length;
    items[next]?.focus();
  }

  focusPrevNavItem(): void {
    const items = this._getNavItems();
    if (items.length === 0) return;
    const idx = items.findIndex((el) => el === document.activeElement);
    const prev = idx <= 0 ? items.length - 1 : idx - 1;
    items[prev]?.focus();
  }

  private _getNavItems(): HTMLElement[] {
    const nav = this.renderRoot.querySelector(".nav");
    if (!nav) return [];
    return Array.from(nav.querySelectorAll<HTMLElement>("button.nav-item"));
  }

  private _magneticInteractionAllowed(): boolean {
    return (
      !this.collapsed &&
      !window.matchMedia("(hover: none)").matches &&
      !window.matchMedia("(prefers-reduced-motion: reduce)").matches
    );
  }

  private _resetMagneticNavItemWraps(nav: HTMLElement): void {
    nav.querySelectorAll<HTMLElement>(".nav-item-wrap").forEach((w) => {
      w.style.transform = "";
    });
  }

  private _teardownMagneticNavListeners(): void {
    if (this._magneticRafId !== null) {
      cancelAnimationFrame(this._magneticRafId);
      this._magneticRafId = null;
    }
    this._magneticPointer = null;
    const nav = this._magneticNav;
    if (nav) {
      nav.removeEventListener("pointermove", this._onMagneticPointerMove);
      nav.removeEventListener("pointerleave", this._onMagneticPointerEnd);
      nav.removeEventListener("pointercancel", this._onMagneticPointerEnd);
      nav.classList.remove("nav--magnetic-tracking");
      this._resetMagneticNavItemWraps(nav);
    }
    this._magneticNav = null;
  }

  private _syncMagneticNavListeners(): void {
    this._teardownMagneticNavListeners();
    if (!this.isConnected || !this._magneticInteractionAllowed()) return;
    const nav = this.renderRoot.querySelector<HTMLElement>(".nav");
    if (!nav) return;
    this._magneticNav = nav;
    nav.addEventListener("pointermove", this._onMagneticPointerMove);
    nav.addEventListener("pointerleave", this._onMagneticPointerEnd);
    nav.addEventListener("pointercancel", this._onMagneticPointerEnd);
  }

  private _flushMagneticFrame(): void {
    this._magneticRafId = null;
    const nav = this._magneticNav;
    const ptr = this._magneticPointer;
    if (!nav || ptr === null || !this._magneticInteractionAllowed()) {
      if (nav) this._resetMagneticNavItemWraps(nav);
      return;
    }
    const py = ptr.clientY;
    const buttons = Array.from(nav.querySelectorAll<HTMLElement>("button.nav-item")).slice(
      0,
      HU_SIDEBAR_MAGNETIC_MAX_ITEMS,
    );
    const radius = HU_SIDEBAR_MAGNETIC_RADIUS_PX;
    const maxShift = HU_SIDEBAR_MAGNETIC_MAX_SHIFT_PX;

    for (const btn of buttons) {
      const wrap = btn.parentElement;
      if (!wrap?.classList.contains("nav-item-wrap")) continue;
      const rect = btn.getBoundingClientRect();
      const cy = rect.top + rect.height * 0.5;
      const dist = Math.abs(py - cy);
      let ty = 0;
      if (dist < radius) {
        const t = 1 - dist / radius;
        const falloff = t * t;
        ty = Math.sign(py - cy) * maxShift * falloff;
      }
      (wrap as HTMLElement).style.transform = ty === 0 ? "" : `translateY(${ty.toFixed(2)}px)`;
    }
  }

  override disconnectedCallback(): void {
    if (this._mqlHoverNone) {
      this._mqlHoverNone.removeEventListener("change", this._onMagneticMql);
      this._mqlHoverNone = null;
    }
    if (this._mqlReduceMotion) {
      this._mqlReduceMotion.removeEventListener("change", this._onMagneticMql);
      this._mqlReduceMotion = null;
    }
    this._teardownMagneticNavListeners();
    this._onNavItemLeave();
    super.disconnectedCallback();
  }

  private _onNavKeyDown(e: KeyboardEvent): void {
    if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
    e.preventDefault();
    const nav = this.renderRoot.querySelector<HTMLElement>(".nav");
    if (!nav) return;
    const buttons = Array.from(nav.querySelectorAll<HTMLElement>(".nav-item"));
    const idx = buttons.indexOf(e.target as HTMLElement);
    if (idx === -1) return;
    const next =
      e.key === "ArrowDown"
        ? (idx + 1) % buttons.length
        : (idx - 1 + buttons.length) % buttons.length;
    buttons[next].focus();
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

  private _dispatchNavHover(tabId: string): void {
    this.dispatchEvent(
      new CustomEvent("nav-hover", {
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
