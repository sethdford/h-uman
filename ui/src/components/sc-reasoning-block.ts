import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./sc-code-block.js";

const PREVIEW_LENGTH = 100;

@customElement("sc-reasoning-block")
export class ScReasoningBlock extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: Boolean }) streaming = false;
  @property({ type: String }) duration = "";
  @property({ type: Boolean }) collapsed = true;

  static override styles = css`
    @keyframes sc-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.5;
      }
    }

    :host {
      display: block;
    }

    .reasoning-block {
      background: color-mix(in srgb, var(--sc-accent) 4%, transparent);
      border-left: 2px solid color-mix(in srgb, var(--sc-accent) 30%, transparent);
      border-radius: var(--sc-radius-md);
      overflow: hidden;
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      user-select: none;
      width: 100%;
      text-align: left;
      background: transparent;
      border: none;
      transition: background-color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .header:hover {
      background: color-mix(in srgb, var(--sc-accent) 8%, transparent);
    }

    .header:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .caret {
      display: inline-flex;
      width: 1em;
      height: 1em;
      flex-shrink: 0;
      transition: transform var(--sc-duration-normal) var(--sc-ease-out);
    }

    .caret.expanded {
      transform: rotate(90deg);
    }

    .label.streaming {
      animation: sc-pulse var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .pulse-dot {
      width: var(--sc-space-xs);
      height: var(--sc-space-xs);
      border-radius: 50%;
      background: var(--sc-accent);
      animation: sc-pulse var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .content {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      padding: 0 var(--sc-space-md) var(--sc-space-md);
      max-height: 2000px;
      overflow: hidden;
      transition: max-height var(--sc-duration-normal) var(--sc-ease-out);
    }

    .content.collapsed {
      max-height: 0;
      padding-top: 0;
      padding-bottom: 0;
      overflow: hidden;
    }

    .preview {
      color: var(--sc-text-muted);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .md-content {
      color: var(--sc-text);
    }

    .md-heading {
      font-weight: var(--sc-weight-semibold);
      margin: var(--sc-space-sm) 0 var(--sc-space-xs);
    }

    .md-heading:first-child {
      margin-top: 0;
    }

    .md-content h1.md-heading {
      font-size: var(--sc-text-2xl);
    }
    .md-content h2.md-heading {
      font-size: var(--sc-text-xl);
    }
    .md-content h3.md-heading {
      font-size: var(--sc-text-lg);
    }
    .md-content h4.md-heading,
    .md-content h5.md-heading,
    .md-content h6.md-heading {
      font-size: var(--sc-text-base);
    }

    .md-paragraph {
      margin: var(--sc-space-xs) 0;
    }

    .md-blockquote {
      border-left: 3px solid var(--sc-accent);
      padding-left: var(--sc-space-md);
      margin: var(--sc-space-sm) 0;
      color: var(--sc-text-muted);
      font-style: italic;
    }

    .md-list {
      margin: var(--sc-space-sm) 0;
      padding-left: var(--sc-space-lg);
    }

    .md-list-item {
      margin-bottom: var(--sc-space-2xs);
    }

    .md-content a {
      color: var(--sc-accent);
      text-decoration: none;
    }

    .md-content a:hover {
      text-decoration: underline;
    }

    .md-content code.inline {
      font-family: var(--sc-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--sc-radius-sm);
      background: var(--sc-bg-inset);
    }

    @media (prefers-reduced-motion: reduce) {
      .caret {
        transition: none;
      }

      .content {
        transition: none;
      }

      .label.streaming,
      .pulse-dot {
        animation: none;
        opacity: 1;
      }
    }
  `;

  private get _preview(): string {
    const text = this.content.trim().replace(/\s+/g, " ");
    if (text.length <= PREVIEW_LENGTH) return text;
    return text.slice(0, PREVIEW_LENGTH) + "...";
  }

  private _toggle() {
    this.collapsed = !this.collapsed;
  }

  private _onKeyDown(e: KeyboardEvent) {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this._toggle();
    }
  }

  override render() {
    const label = this.duration || "Thinking";
    return html`
      <div class="reasoning-block" role="region" aria-label="AI reasoning">
        <button
          class="header"
          @click=${this._toggle}
          @keydown=${this._onKeyDown}
          aria-expanded=${!this.collapsed}
          aria-controls="reasoning-content"
          aria-label="Toggle reasoning content"
        >
          <span class="caret ${this.collapsed ? "" : "expanded"}">${icons["caret-right"]}</span>
          <span class="label ${this.streaming ? "streaming" : ""}">${label}</span>
          ${this.streaming ? html`<span class="pulse-dot" aria-hidden="true"></span>` : nothing}
        </button>
        <div
          id="reasoning-content"
          class="content ${this.collapsed ? "collapsed" : "expanded"}"
          role=${this.streaming ? "status" : nothing}
          aria-live=${this.streaming ? "polite" : nothing}
        >
          ${this.collapsed
            ? html`<span class="preview">${this._preview}</span>`
            : renderMarkdown(this.content, { streaming: this.streaming })}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-reasoning-block": ScReasoningBlock;
  }
}
