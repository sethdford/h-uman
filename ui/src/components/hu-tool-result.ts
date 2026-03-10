import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

type ToolStatus = "running" | "success" | "error";

@customElement("hu-tool-result")
export class ScToolResult extends LitElement {
  @property({ type: String }) tool = "";
  @property({ type: String }) status: ToolStatus = "running";
  @property({ type: String }) content = "";
  @property({ type: Boolean }) collapsed = false;

  static override styles = css`
    @keyframes hu-spin {
      to {
        transform: rotate(360deg);
      }
    }

    @keyframes hu-pulse-border {
      0%,
      100% {
        border-left-color: var(--hu-info);
      }
      50% {
        border-left-color: color-mix(in srgb, var(--hu-info) 30%, transparent);
      }
    }

    :host {
      display: block;
    }

    .container {
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-left: 0.1875rem solid var(--hu-info);
      border-radius: var(--hu-radius);
      box-shadow: var(--hu-shadow-xs);
      overflow: hidden;
    }

    .container.status-running {
      animation: hu-pulse-border var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .container.status-success {
      border-left-color: var(--hu-success);
      animation: none;
    }

    .container.status-error {
      border-left-color: var(--hu-error);
      animation: none;
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
      cursor: pointer;
      user-select: none;
    }

    .header:hover {
      background: color-mix(in srgb, var(--hu-surface, var(--hu-bg-surface)) 8%, transparent);
    }

    .header:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: -2px;
    }

    .icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      flex-shrink: 0;
      color: inherit;
    }

    .icon.running {
      animation: hu-spin var(--hu-duration-slow) linear infinite;
    }

    .icon.success {
      color: var(--hu-accent);
    }

    .icon.error {
      color: var(--hu-error);
    }

    .tool-body {
      overflow: hidden;
      transition: max-height var(--hu-duration-normal) var(--hu-ease-out);
    }

    .content {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
      padding: var(--hu-space-md);
      border-top: 1px solid var(--hu-border-subtle);
      overflow-x: auto;
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 18.75rem;
      overflow-y: auto;
      transition: max-height var(--hu-duration-normal) var(--hu-ease-out);
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
    "hu-tool-result": ScToolResult;
  }
}
