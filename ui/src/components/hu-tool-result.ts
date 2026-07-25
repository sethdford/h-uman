import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import type { PropertyValues } from "lit";
import { icons } from "../icons.js";

type ToolStatus = "running" | "success" | "error";

/** Escape then wrap JSON token spans for shadow DOM (no external HTML). */
function highlightJsonToHtml(json: string): string {
  let i = 0;
  let out = "";
  const s = json;
  const esc = (t: string) => t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  const span = (cls: string, raw: string) => {
    out += `<span class="${cls}">${esc(raw)}</span>`;
  };

  while (i < s.length) {
    const c = s[i];
    if (/\s/.test(c)) {
      let j = i;
      while (j < s.length && /\s/.test(s[j])) j++;
      out += esc(s.slice(i, j));
      i = j;
      continue;
    }
    if (c === '"') {
      let j = i + 1;
      while (j < s.length) {
        if (s[j] === "\\") {
          j += 2;
          continue;
        }
        if (s[j] === '"') break;
        j++;
      }
      const token = s.slice(i, j + 1);
      let k = j + 1;
      while (k < s.length && /\s/.test(s[k])) k++;
      if (s[k] === ":") span("json-key", token);
      else span("json-str", token);
      i = j + 1;
      continue;
    }
    if (c === "-" || (c >= "0" && c <= "9")) {
      let j = i;
      if (s[j] === "-") j++;
      while (j < s.length && s[j] >= "0" && s[j] <= "9") j++;
      if (s[j] === ".") {
        j++;
        while (j < s.length && s[j] >= "0" && s[j] <= "9") j++;
      }
      if (s[j] === "e" || s[j] === "E") {
        j++;
        if (s[j] === "+" || s[j] === "-") j++;
        while (j < s.length && s[j] >= "0" && s[j] <= "9") j++;
      }
      span("json-num", s.slice(i, j));
      i = j;
      continue;
    }
    if (s.startsWith("true", i)) {
      span("json-lit", "true");
      i += 4;
      continue;
    }
    if (s.startsWith("false", i)) {
      span("json-lit", "false");
      i += 5;
      continue;
    }
    if (s.startsWith("null", i)) {
      span("json-lit", "null");
      i += 4;
      continue;
    }
    out += esc(c);
    i++;
  }
  return out;
}

@customElement("hu-tool-result")
export class ScToolResult extends LitElement {
  @property({ type: String }) tool = "";
  @property({ type: String }) status: ToolStatus = "running";
  /** Tool output / result text (legacy: sole body when `input` is unset). */
  @property({ type: String }) content = "";
  /** JSON string of tool arguments; when set, enables input/output sections. */
  @property({ type: String }) input = "";
  @property({ type: Boolean }) collapsed = false;

  @state() private _inputCollapsed = true;
  @state() private _outputCollapsed = false;
  @state() private _elapsedMs = 0;
  @state() private _enterActive = true;
  @state() private _flashActive = false;

  private static _nextInstanceId = 0;
  private readonly _instanceId = ++ScToolResult._nextInstanceId;

  private _runStart = 0;
  private _elapsedInterval: ReturnType<typeof setInterval> | undefined;

  static override styles = css`
    @keyframes hu-spin {
      to {
        transform: rotate(360deg);
      }
    }

    @keyframes hu-tool-running-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.7;
      }
    }

    @keyframes hu-tool-enter {
      from {
        opacity: 0;
        transform: translateY(calc(-1 * var(--hu-space-sm)));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    @keyframes hu-status-flash {
      0% {
        border-left-color: var(--current-status-color);
        box-shadow: inset 0.125rem 0 var(--hu-space-xs)
          color-mix(in srgb, var(--current-status-color) 20%, transparent);
      }
      100% {
        border-left-color: var(--current-status-color);
        box-shadow: none;
      }
    }

    @keyframes hu-header-shimmer {
      from {
        transform: translateX(-100%);
      }
      to {
        transform: translateX(100%);
      }
    }

    :host {
      display: block;
    }

    .container {
      --current-status-color: var(--hu-info);
      background: var(--hu-surface-container);
      border: 1px solid var(--hu-border-subtle);
      border-left: 0.1875rem solid var(--current-status-color);
      border-radius: var(--hu-radius);
      overflow: hidden;
      animation: hu-tool-enter var(--hu-duration-normal) var(--hu-ease-out) forwards;

      &.status-running {
        --current-status-color: var(--hu-info);
        animation:
          hu-tool-enter var(--hu-duration-normal) var(--hu-ease-out) forwards,
          hu-tool-running-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
      }
      &.status-success {
        --current-status-color: var(--hu-success);
      }
      &.status-error {
        --current-status-color: var(--hu-error);
      }

      &.status-flash {
        animation: hu-status-flash var(--hu-duration-normal) var(--hu-ease-out);
      }
    }

    .header {
      position: relative;
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
      user-select: none;
    }

    .header--interactive {
      cursor: pointer;
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .header--interactive:hover {
      background: color-mix(in srgb, var(--hu-surface, var(--hu-bg-surface)) 8%, transparent);
    }

    .header--interactive:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: calc(var(--hu-focus-ring-offset) * -1);
    }

    .header-row {
      display: flex;
      align-items: flex-start;
      gap: var(--hu-space-sm);
    }

    .header-titles {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
      min-width: 0;
      flex: 1;
    }

    .title-line {
      display: flex;
      align-items: center;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      min-width: 0;
    }

    .tool-name {
      font-weight: var(--hu-weight-semibold);
      word-break: break-word;
    }

    .wrench-icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      flex-shrink: 0;
      color: var(--hu-text-muted);
    }

    .subtitle {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-regular);
      color: var(--hu-text-muted);
    }

    .icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      flex-shrink: 0;
      color: inherit;
      margin-top: var(--hu-space-3xs);
    }

    .icon.running {
      animation: hu-spin var(--hu-duration-slow) linear infinite;
    }

    .icon.success {
      color: var(--hu-success);
    }

    .icon.error {
      color: var(--hu-error);
    }

    .header-progress {
      position: absolute;
      left: 0;
      right: 0;
      bottom: 0;
      height: var(--hu-space-3xs);
      overflow: hidden;
      pointer-events: none;
      background: color-mix(in srgb, var(--hu-info) 12%, transparent);
    }

    .header-progress-bar {
      position: absolute;
      inset: 0;
      width: 40%;
      background: linear-gradient(
        90deg,
        transparent,
        color-mix(in srgb, var(--hu-info) 55%, transparent),
        transparent
      );
      animation: hu-header-shimmer var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .section {
      border-top: 1px solid var(--hu-border-subtle);
    }

    .section-toggle {
      display: flex;
      align-items: center;
      justify-content: space-between;
      width: 100%;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      margin: 0;
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      text-align: left;
      color: var(--hu-text-muted);
      background: transparent;
      border: none;
      cursor: pointer;
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .section-toggle:hover {
      background: color-mix(in srgb, var(--hu-surface, var(--hu-bg-surface)) 6%, transparent);
    }

    .section-toggle:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: calc(var(--hu-focus-ring-offset) * -1);
    }

    .section-toggle--output-success {
      color: var(--hu-success);
    }

    .section-toggle--output-error {
      color: var(--hu-error);
    }

    .section-chevron {
      flex-shrink: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      color: color-mix(in srgb, currentColor 70%, transparent);
    }

    .section-body {
      overflow: hidden;
      transition:
        max-height var(--hu-duration-normal) var(--hu-ease-out),
        opacity var(--hu-duration-normal) var(--hu-ease-out);
    }

    .section-body--collapsed {
      max-height: 0 !important;
      opacity: 0;
      overflow: hidden;
      pointer-events: none;
    }

    .section-body--open {
      max-height: var(--hu-tool-result-body-max, 18.75rem);
      opacity: 1;
    }

    .code-block {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
      padding: 0 var(--hu-space-md) var(--hu-space-md);
      overflow-x: auto;
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 18.75rem;
      overflow-y: auto;
    }

    .code-block--legacy {
      border-top: 1px solid var(--hu-border-subtle);
      padding: var(--hu-space-md);
    }

    .code-block.collapsed {
      max-height: 0;
      padding-top: 0;
      padding-bottom: 0;
      border-top-width: 0;
      overflow: hidden;
    }

    .json-key {
      color: var(--hu-accent-tertiary);
    }

    .json-str {
      color: var(--hu-success);
    }

    .json-num,
    .json-lit {
      color: var(--hu-accent-secondary);
    }

    @media (prefers-reduced-motion: reduce) {
      .container.container--enter,
      .container.container--enter.status-running,
      .container.status-running:not(.container--enter) {
        animation: none;
      }

      .container.status-running {
        border-left-color: var(--hu-info);
      }

      .container.status-flash {
        animation: none;
      }

      .header-progress-bar {
        animation: none;
        transform: none;
        left: 0;
        width: 100%;
        background: color-mix(in srgb, var(--hu-info) 35%, transparent);
      }

      .icon.running {
        animation: none;
      }

      .section-body {
        transition: none;
      }

      .code-block {
        transition: none;
      }
    }
  `;

  private get _hasInputSection(): boolean {
    return this.input.trim().length > 0;
  }

  private _clearElapsedInterval(): void {
    if (this._elapsedInterval !== undefined) {
      clearInterval(this._elapsedInterval);
      this._elapsedInterval = undefined;
    }
  }

  private _syncRunningTimer(): void {
    if (this.status !== "running") {
      this._clearElapsedInterval();
      this._elapsedMs = 0;
      return;
    }
    if (this._elapsedInterval !== undefined) return;
    this._runStart = Date.now();
    this._elapsedMs = 0;
    this._elapsedInterval = setInterval(() => {
      this._elapsedMs = Date.now() - this._runStart;
      this.requestUpdate();
    }, 100);
  }

  override connectedCallback(): void {
    super.connectedCallback();
    if (
      typeof globalThis !== "undefined" &&
      globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches
    ) {
      this._enterActive = false;
    }
    this._syncRunningTimer();
  }

  override disconnectedCallback(): void {
    this._clearElapsedInterval();
    super.disconnectedCallback();
  }

  override firstUpdated(changed: PropertyValues): void {
    super.firstUpdated(changed);
    if (!this._enterActive) return;
    const el = this.shadowRoot?.querySelector(".container") as HTMLElement | null;
    if (!el) return;
    const onEnd = (e: AnimationEvent) => {
      if (e.animationName !== "hu-tool-enter") return;
      this._enterActive = false;
      el.removeEventListener("animationend", onEnd);
    };
    el.addEventListener("animationend", onEnd);
  }

  override willUpdate(changed: PropertyValues): void {
    if (changed.has("status")) {
      const prev = changed.get("status") as ToolStatus | undefined;
      if (prev === "running" && (this.status === "success" || this.status === "error")) {
        const reduce =
          typeof globalThis !== "undefined" &&
          globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
        if (!reduce) this._flashActive = true;
      }
      this._syncRunningTimer();
    }
  }

  override updated(changed: PropertyValues): void {
    super.updated(changed);
    if (changed.has("_flashActive") && this._flashActive) {
      requestAnimationFrame(() => {
        const el = this.shadowRoot?.querySelector(".container") as HTMLElement | null;
        if (!el) return;
        const onEnd = (e: Event) => {
          if ((e as AnimationEvent).animationName !== "hu-status-flash") return;
          this._flashActive = false;
          el.removeEventListener("animationend", onEnd);
        };
        el.addEventListener("animationend", onEnd);
      });
    }
  }

  private _toggleLegacy() {
    this.collapsed = !this.collapsed;
  }

  private _onLegacyKeyDown(e: KeyboardEvent) {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this._toggleLegacy();
    }
  }

  private _toggleInput() {
    this._inputCollapsed = !this._inputCollapsed;
  }

  private _toggleOutput() {
    this._outputCollapsed = !this._outputCollapsed;
  }

  private _statusSubtitle(): string {
    if (this.status === "running") {
      const sec = (this._elapsedMs / 1000).toFixed(1);
      return `Running... ${sec}s`;
    }
    if (this.status === "success") return "Completed";
    return "Error";
  }

  private _statusIcon() {
    switch (this.status) {
      case "running":
        return html`<span class="icon running" aria-hidden="true">${icons.refresh}</span>`;
      case "success":
        return html`<span class="icon success" aria-hidden="true">${icons.check}</span>`;
      case "error":
        return html`<span class="icon error" aria-hidden="true">${icons.x}</span>`;
      default:
        return html`<span class="icon" aria-hidden="true">${icons.refresh}</span>`;
    }
  }

  private _renderHeader() {
    const interactive = !this._hasInputSection;
    return html`
      <div
        class="header ${interactive ? "header--interactive" : ""}"
        role=${interactive ? "button" : "region"}
        tabindex=${interactive ? 0 : undefined}
        aria-expanded=${interactive ? (!this.collapsed).toString() : undefined}
        aria-label=${interactive ? `Toggle ${this.tool} result` : `Tool ${this.tool}`}
        @click=${interactive ? this._toggleLegacy : undefined}
        @keydown=${interactive ? this._onLegacyKeyDown : undefined}
      >
        <div class="header-row">
          ${this._statusIcon()}
          <span class="wrench-icon" aria-hidden="true">${icons.wrench}</span>
          <div class="header-titles">
            <div class="title-line">
              <span class="tool-name">${this.tool}</span>
            </div>
            <span class="subtitle">${this._statusSubtitle()}</span>
          </div>
        </div>
        ${
          this.status === "running"
            ? html`
                <div class="header-progress" aria-hidden="true">
                  <div class="header-progress-bar"></div>
                </div>
              `
            : nothing
        }
      </div>
    `;
  }

  private _renderInputSection() {
    const expanded = !this._inputCollapsed;
    const htmlBody = highlightJsonToHtml(this.input);
    const bodyId = `hu-tool-input-${this._instanceId}`;
    const toggleId = `hu-tool-input-toggle-${this._instanceId}`;
    return html`
      <div class="section">
        <button
          type="button"
          class="section-toggle"
          aria-expanded=${expanded}
          aria-controls=${bodyId}
          id=${toggleId}
          @click=${this._toggleInput}
        >
          <span>Input</span>
          <span class="section-chevron" aria-hidden="true"
            >${expanded ? icons["caret-down"] : icons["caret-right"]}</span
          >
        </button>
        <div
          id=${bodyId}
          role="region"
          aria-labelledby=${toggleId}
          class="section-body ${expanded ? "section-body--open" : "section-body--collapsed"}"
        >
          <pre class="code-block">${unsafeHTML(htmlBody)}</pre>
        </div>
      </div>
    `;
  }

  private _renderOutputSection() {
    const expanded = !this._outputCollapsed;
    const err = this.status === "error";
    const bodyId = `hu-tool-output-${this._instanceId}`;
    const toggleId = `hu-tool-output-toggle-${this._instanceId}`;
    return html`
      <div class="section">
        <button
          type="button"
          class="section-toggle ${
            err ? "section-toggle--output-error" : "section-toggle--output-success"
          }"
          aria-expanded=${expanded}
          aria-controls=${bodyId}
          id=${toggleId}
          @click=${this._toggleOutput}
        >
          <span>Output</span>
          <span class="section-chevron" aria-hidden="true"
            >${expanded ? icons["caret-down"] : icons["caret-right"]}</span
          >
        </button>
        <div
          id=${bodyId}
          role="region"
          aria-labelledby=${toggleId}
          class="section-body ${expanded ? "section-body--open" : "section-body--collapsed"}"
        >
          <div class="code-block">${this.content}</div>
        </div>
      </div>
    `;
  }

  private _renderLegacyBody() {
    return html`
      <div
        class="code-block code-block--legacy tool-body ${this.collapsed ? "collapsed" : ""}"
        aria-hidden=${this.collapsed}
      >
        ${this.content}
      </div>
    `;
  }

  override render() {
    const statusClass = `status-${this.status}`;
    const showOutput =
      this._hasInputSection && (this.status === "success" || this.status === "error");

    return html`
      <div
        class="container ${statusClass} ${this._enterActive ? "container--enter" : ""} ${
          this._flashActive ? "status-flash" : ""
        }"
      >
        ${this._renderHeader()}
        ${
          this._hasInputSection
            ? html`${this._renderInputSection()}${showOutput ? this._renderOutputSection() : nothing}`
            : this._renderLegacyBody()
        }
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-tool-result": ScToolResult;
  }
}
