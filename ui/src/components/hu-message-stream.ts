import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./hu-code-block.js";
import "./hu-toast.js";
import { ScToast } from "./hu-toast.js";

type MessageRole = "user" | "assistant";

@customElement("hu-message-stream")
export class ScMessageStream extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: Boolean }) streaming = false;
  @property({ type: String }) role: MessageRole = "assistant";

  static override styles = css`
    @keyframes hu-slide-up {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-sm));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    :host {
      display: block;
    }

    .bubble {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      line-height: var(--hu-leading-relaxed);
      padding: var(--hu-space-md) var(--hu-space-lg);
      border-radius: var(--hu-radius-xl);
      max-width: 85%;
      animation: hu-slide-up var(--hu-duration-fast) var(--hu-ease-out) both;
    }

    .bubble.role-user {
      margin-left: auto;
      background: color-mix(in srgb, var(--hu-accent) 12%, var(--hu-bg-surface));
      border: 1px solid color-mix(in srgb, var(--hu-accent) 20%, var(--hu-border-subtle));
    }

    .bubble.role-assistant {
      margin-right: auto;
      position: relative;
      background: color-mix(
        in srgb,
        var(--hu-bg-surface) var(--hu-glass-standard-bg-opacity, 6%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur, 24px))
        saturate(var(--hu-glass-standard-saturate, 180%));
      border: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 8%),
          transparent
        );
    }

    .bubble.role-assistant::after {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      background: radial-gradient(
        circle at var(--hu-light-x, 50%) var(--hu-light-y, 50%),
        color-mix(in srgb, white 4%, transparent),
        transparent 60%
      );
      pointer-events: none;
      mix-blend-mode: overlay;
      z-index: 0;
    }

    @media (prefers-reduced-transparency: reduce) {
      .bubble.role-assistant {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-bg-surface);
        border: 1px solid var(--hu-border);
      }
      .bubble.role-assistant::after {
        display: none;
      }
    }

    .content {
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
      border-left: 3px solid var(--hu-accent);
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

    .md-table {
      border-collapse: collapse;
      width: 100%;
      margin: var(--hu-space-sm) 0;
    }

    .md-table th,
    .md-table td {
      border: 1px solid var(--hu-border);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      text-align: left;
    }

    .md-table th {
      background: var(--hu-bg-elevated);
      font-weight: var(--hu-weight-semibold);
    }

    .md-hr {
      border: none;
      border-top: 1px solid var(--hu-border);
      margin: var(--hu-space-md) 0;
    }

    .md-content a {
      color: var(--hu-accent);
      text-decoration: none;
    }

    .md-content a:hover {
      text-decoration: underline;
    }

    .md-content img {
      max-width: 100%;
      border-radius: var(--hu-radius-md);
    }

    .md-content code.inline {
      font-family: var(--hu-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--hu-radius-sm);
      background: var(--hu-bg-inset);
    }

    .cursor {
      display: inline-block;
      width: 2px;
      height: 1em;
      background: var(--hu-accent);
      margin-left: var(--hu-space-2xs);
      vertical-align: text-bottom;
      animation: hu-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    @media (prefers-reduced-motion: reduce) {
      .bubble {
        animation: none;
      }
      .cursor {
        animation: none;
      }
    }
  `;

  private _copyCode(code: string): void {
    navigator.clipboard?.writeText(code).then(
      () => {
        ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      },
      () => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      },
    );
  }

  override render() {
    return html`
      <div class="bubble role-${this.role}">
        <div class="content">
          ${renderMarkdown(this.content, {
            onCopyCode: (code) => this._copyCode(code),
            streaming: this.streaming,
          })}
          ${this.streaming ? html`<span class="cursor" aria-hidden="true"></span>` : nothing}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-message-stream": ScMessageStream;
  }
}
