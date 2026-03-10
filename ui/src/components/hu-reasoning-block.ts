import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./hu-code-block.js";

const PREVIEW_LENGTH = 100;

@customElement("hu-reasoning-block")
export class ScReasoningBlock extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: Boolean }) streaming = false;
  @property({ type: String }) duration = "";
  @property({ type: Boolean }) collapsed = true;

  static override styles = css`
    @keyframes hu-pulse {
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
      background: color-mix(in srgb, var(--hu-accent) 4%, transparent);
      border-left: 2px solid color-mix(in srgb, var(--hu-accent) 30%, transparent);
      border-radius: var(--hu-radius-md);
      overflow: hidden;
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      user-select: none;
      width: 100%;
      text-align: left;
      background: transparent;
      border: none;
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .header:hover {
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
    }

    .header:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .caret {
      display: inline-flex;
      width: 1em;
      height: 1em;
      flex-shrink: 0;
      transition: transform var(--hu-duration-normal) var(--hu-ease-out);
    }

    .caret.expanded {
      transform: rotate(90deg);
    }

    .label.streaming {
      animation: hu-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .pulse-dot {
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: 50%;
      background: var(--hu-accent);
      animation: hu-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .content {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      padding: 0 var(--hu-space-md) var(--hu-space-md);
      max-height: 125rem;
      overflow: hidden;
      transition: max-height var(--hu-duration-normal) var(--hu-ease-out);
    }

    .content.collapsed {
      max-height: 0;
      padding-top: 0;
      padding-bottom: 0;
      overflow: hidden;
    }

    .preview {
      color: var(--hu-text-muted);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .md-content {
      color: var(--hu-text);
    }

    .md-heading {
      font-weight: var(--hu-weight-semibold);
      margin: var(--hu-space-sm) 0 var(--hu-space-xs);
    }

    .md-heading:first-child {
      margin-top: 0;
    }

    .md-content h1.md-heading {
      font-size: var(--hu-text-2xl);
    }
    .md-content h2.md-heading {
      font-size: var(--hu-text-xl);
    }
    .md-content h3.md-heading {
      font-size: var(--hu-text-lg);
    }
    .md-content h4.md-heading,
    .md-content h5.md-heading,
    .md-content h6.md-heading {
      font-size: var(--hu-text-base);
    }

    .md-paragraph {
      margin: var(--hu-space-xs) 0;
    }

    .md-blockquote {
      border-left: 0.1875rem solid var(--hu-accent);
      padding-left: var(--hu-space-md);
      margin: var(--hu-space-sm) 0;
      color: var(--hu-text-muted);
      font-style: italic;
    }

    .md-list {
      margin: var(--hu-space-sm) 0;
      padding-left: var(--hu-space-lg);
    }

    .md-list-item {
      margin-bottom: var(--hu-space-2xs);
    }

    .md-content a {
      color: var(--hu-accent);
      text-decoration: none;
    }

    .md-content a:hover {
      text-decoration: underline;
    }

    .md-content code.inline {
      font-family: var(--hu-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--hu-radius-sm);
      background: var(--hu-bg-inset);
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
    "hu-reasoning-block": ScReasoningBlock;
  }
}
