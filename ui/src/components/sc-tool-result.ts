import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

type ToolStatus = "running" | "success" | "error";

@customElement("sc-tool-result")
export class ScToolResult extends LitElement {
  @property({ type: String }) tool = "";
  @property({ type: String }) status: ToolStatus = "running";
  @property({ type: String }) content = "";
  @property({ type: Boolean }) collapsed = false;

  static override styles = css`
    @keyframes sc-spin {
      to {
        transform: rotate(360deg);
      }
    }

    @keyframes sc-pulse-border {
      0%,
      100% {
        border-left-color: var(--sc-info);
      }
      50% {
        border-left-color: color-mix(in srgb, var(--sc-info) 30%, transparent);
      }
    }

    :host {
      display: block;
    }

    .container {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-left: 0.1875rem solid var(--sc-info);
      border-radius: var(--sc-radius);
      box-shadow: var(--sc-shadow-xs);
      overflow: hidden;
    }

    .container.status-running {
      animation: sc-pulse-border var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .container.status-success {
      border-left-color: var(--sc-success);
      animation: none;
    }

    .container.status-error {
      border-left-color: var(--sc-error);
      animation: none;
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
      cursor: pointer;
      user-select: none;
    }

    .header:hover {
      background: color-mix(in srgb, var(--sc-surface, var(--sc-bg-surface)) 8%, transparent);
    }

    .header:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: -2px;
    }

    .icon {
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      flex-shrink: 0;
      color: inherit;
    }

    .icon.running {
      animation: sc-spin var(--sc-duration-slow) linear infinite;
    }

    .icon.success {
      color: var(--sc-accent);
    }

    .icon.error {
      color: var(--sc-error);
    }

    .tool-body {
      overflow: hidden;
      transition: max-height var(--sc-duration-normal) var(--sc-ease-out);
    }

    .content {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-xs);
      color: var(--sc-text);
      padding: var(--sc-space-md);
      border-top: 1px solid var(--sc-border-subtle);
      overflow-x: auto;
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 18.75rem;
      overflow-y: auto;
      transition: max-height var(--sc-duration-normal) var(--sc-ease-out);
    }

    .content.collapsed {
      max-height: 0;
      padding-top: 0;
      padding-bottom: 0;
      border-top-width: 0;
      overflow: hidden;
    }

    @media (prefers-reduced-motion: reduce) {
      .container.status-running {
        animation: none;
      }

      .tool-body,
      .content {
        transition: none;
      }

      .icon.running {
        animation: none;
      }
    }
  `;

  private _toggle() {
    this.collapsed = !this.collapsed;
  }

  private _onKeyDown(e: KeyboardEvent) {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this._toggle();
    }
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

  override render() {
    const statusClass = `status-${this.status}`;
    return html`
      <div class="container ${statusClass}">
        <div
          class="header"
          role="button"
          tabindex="0"
          aria-expanded=${!this.collapsed}
          aria-label="Toggle ${this.tool} result"
          @click=${this._toggle}
          @keydown=${this._onKeyDown}
        >
          ${this._statusIcon()}
          <span>${this.tool}</span>
        </div>
        <div class="content tool-body ${this.collapsed ? "collapsed" : ""}">${this.content}</div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-tool-result": ScToolResult;
  }
}
