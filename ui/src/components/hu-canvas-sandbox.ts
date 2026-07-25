import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { buildHarness } from "../canvas-harness.js";

/**
 * Renders canvas content in a sandboxed iframe. No same-origin access,
 * no popups, no top-navigation. Communication via postMessage only.
 */
@customElement("hu-canvas-sandbox")
export class HuCanvasSandbox extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: String }) format: string = "html";
  @property({ type: Object }) imports: Record<string, string> = {};
  @property({ type: String }) language = "";

  @state() private _ready = false;
  @state() private _error = "";
  @state() private _height = 200;

  private _iframe: HTMLIFrameElement | null = null;
  @state() private _harnessSrc = "";
  private _messageHandler = this._onMessage.bind(this);
  private _pendingRender = false;

  static override styles = css`
    :host {
      display: block;
      min-height: 8rem;
      position: relative;
    }

    iframe {
      border: none;
      width: 100%;
      display: block;
      background: transparent;
    }

    .loading {
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 8rem;
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
    }

    .error-banner {
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: color-mix(in srgb, var(--hu-error) 12%, transparent);
      border: 1px solid color-mix(in srgb, var(--hu-error) 30%, transparent);
      border-radius: var(--hu-radius-md);
      color: var(--hu-error);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font-mono);
      margin: var(--hu-space-sm);
      white-space: pre-wrap;
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    window.addEventListener("message", this._messageHandler);
  }

  override disconnectedCallback(): void {
    window.removeEventListener("message", this._messageHandler);
    super.disconnectedCallback();
  }

  private _onMessage(e: MessageEvent): void {
    if (!this._iframe || e.source !== this._iframe.contentWindow) return;
    const d = e.data;
    if (!d || typeof d !== "object") return;

    switch (d.type) {
      case "canvas:ready":
        this._ready = true;
        this._error = "";
        this._sendRender();
        break;
      case "canvas:error":
        this._error = String(d.error || "Unknown error");
        break;
      case "canvas:resize":
        if (typeof d.height === "number" && d.height > 0) {
          this._height = Math.max(100, Math.min(d.height + 16, 4000));
        }
        break;
    }
  }

  private _sendRender(): void {
    if (!this._iframe?.contentWindow || !this._ready) {
      this._pendingRender = true;
      return;
    }
    this._iframe.contentWindow.postMessage(
      {
        type: "canvas:render",
        content: this.content,
        format: this.format,
        imports: this.imports,
        language: this.language,
      },
      "*",
    );
    this._pendingRender = false;
  }

  override updated(changed: Map<string, unknown>): void {
    if (
      changed.has("content") ||
      changed.has("format") ||
      changed.has("imports") ||
      changed.has("language")
    ) {
      this._sendRender();
    }
  }

  override async firstUpdated(): Promise<void> {
    this._harnessSrc = buildHarness();
    await this.updateComplete;
    this._iframe = this.renderRoot.querySelector("iframe");
    if (this._pendingRender) {
      this._sendRender();
    }
  }

  override render() {
    return html`
      ${this._error ? html`<div class="error-banner" role="alert">${this._error}</div>` : ""}
      ${
        this._harnessSrc
          ? html`<iframe
              sandbox="allow-scripts"
              srcdoc=${this._harnessSrc}
              style="height: ${this._height}px"
              title="Canvas preview"
              aria-label="Sandboxed canvas content preview"
            ></iframe>`
          : html`<div class="loading">Initializing sandbox...</div>`
      }
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-canvas-sandbox": HuCanvasSandbox;
  }
}
