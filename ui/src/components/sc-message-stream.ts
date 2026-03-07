import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./sc-code-block.js";
import "./sc-toast.js";
import { ScToast } from "./sc-toast.js";

type MessageRole = "user" | "assistant";

@customElement("sc-message-stream")
export class ScMessageStream extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: Boolean }) streaming = false;
  @property({ type: String }) role: MessageRole = "assistant";

  static override styles = css`
    @keyframes sc-blink {
      0%,
      50% {
        opacity: 1;
      }
      51%,
      100% {
        opacity: 0;
      }
    }

    :host {
      display: block;
    }

    .bubble {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      line-height: var(--sc-leading-relaxed);
      padding: var(--sc-space-md) var(--sc-space-lg);
      border-radius: var(--sc-radius-xl);
      max-width: 85%;
    }

    .bubble.role-user {
      margin-left: auto;
      background: color-mix(in srgb, var(--sc-accent) 12%, var(--sc-bg-surface));
      border: 1px solid color-mix(in srgb, var(--sc-accent) 20%, var(--sc-border-subtle));
    }

    .bubble.role-assistant {
      margin-right: auto;
      position: relative;
      background: color-mix(
        in srgb,
        var(--sc-bg-surface) var(--sc-glass-standard-bg-opacity, 6%),
        transparent
      );
      backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      border: 1px solid
        color-mix(
          in srgb,
          var(--sc-border) var(--sc-glass-standard-border-opacity, 8%),
          transparent
        );
    }

    .bubble.role-assistant::after {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      background: radial-gradient(
        circle at var(--sc-light-x, 50%) var(--sc-light-y, 50%),
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
        background: var(--sc-bg-surface);
        border: 1px solid var(--sc-border);
      }
      .bubble.role-assistant::after {
        display: none;
      }
    }

    .content {
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

    .md-table {
      border-collapse: collapse;
      width: 100%;
      margin: var(--sc-space-sm) 0;
    }

    .md-table th,
    .md-table td {
      border: 1px solid var(--sc-border);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      text-align: left;
    }

    .md-table th {
      background: var(--sc-bg-elevated);
      font-weight: var(--sc-weight-semibold);
    }

    .md-hr {
      border: none;
      border-top: 1px solid var(--sc-border);
      margin: var(--sc-space-md) 0;
    }

    .md-content a {
      color: var(--sc-accent);
      text-decoration: none;
    }

    .md-content a:hover {
      text-decoration: underline;
    }

    .md-content img {
      max-width: 100%;
      border-radius: var(--sc-radius-md);
    }

    .md-content code.inline {
      font-family: var(--sc-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--sc-radius-sm);
      background: var(--sc-bg-inset);
    }

    .cursor {
      display: inline-block;
      width: 2px;
      height: 1em;
      background: var(--sc-accent);
      margin-left: 2px;
      vertical-align: text-bottom;
      animation: sc-blink 1s step-end infinite;
    }

    @media (prefers-reduced-motion: reduce) {
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
    "sc-message-stream": ScMessageStream;
  }
}
