import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import DOMPurify from "dompurify";
import "./hu-code-block.js";
import "./hu-toast.js";
import { ScToast } from "./hu-toast.js";
import { renderMarkdown } from "../lib/markdown.js";
import { icons } from "../icons.js";

export type ArtifactViewerType = "code" | "document" | "html" | "diagram";

@customElement("hu-artifact-viewer")
export class ScArtifactViewer extends LitElement {
  @property({ type: String }) type: ArtifactViewerType = "code";
  @property({ type: String }) content = "";
  @property({ type: String }) language = "";
  @property({ type: Boolean }) diffMode = false;
  @property({ type: String }) previousContent = "";

  static override styles = css`
    :host {
      display: block;
      height: 100%;
      overflow: hidden;
    }
    .viewer {
      display: flex;
      flex-direction: column;
      height: 100%;
      overflow: hidden;
    }
    .toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-border) 20%, var(--hu-bg-inset));
      border-bottom: 1px solid var(--hu-border-subtle);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
    }
    .type-label {
      text-transform: capitalize;
    }
    .copy-btn {
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: transparent;
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
    }
    .copy-btn:hover {
      color: var(--hu-text);
      border-color: var(--hu-border);
    }
    .copy-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .copy-btn svg {
      width: 0.875rem;
      height: 0.875rem;
    }
    .body {
      flex: 1;
      overflow: auto;
      padding: var(--hu-space-md);
    }
    .body pre {
      margin: 0;
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      white-space: pre-wrap;
      word-break: break-word;
    }
    .body .md-content {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
    }
    .iframe-wrap {
      width: 100%;
      height: 100%;
      min-height: 20rem;
      border: none;
      border-radius: var(--hu-radius-md);
      background: var(--hu-bg-surface);
    }
    @media (prefers-reduced-motion: reduce) {
      .copy-btn {
        transition: none;
      }
    }
  `;

  private _onCopy(): void {
    const text = this.content || "";
    if (!text) return;
    navigator.clipboard
      ?.writeText(text)
      .then(() => {
        ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      })
      .catch(() => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      });
  }

  private _getTypeLabel(): string {
    switch (this.type) {
      case "code":
        return this.language || "Code";
      case "document":
        return "Document";
      case "html":
        return "HTML";
      case "diagram":
        return "Diagram";
      default:
        return this.type;
    }
  }

  private _renderBody() {
    switch (this.type) {
      case "code":
        return html`<hu-code-block
          .code=${this.content}
          .language=${this.language}
        ></hu-code-block>`;
      case "document":
        return html`<div class="body md-content">${renderMarkdown(this.content)}</div>`;
      case "html": {
        const sanitized = DOMPurify.sanitize(this.content, {
          ALLOWED_TAGS: [
            "div",
            "span",
            "p",
            "h1",
            "h2",
            "h3",
            "h4",
            "h5",
            "h6",
            "a",
            "strong",
            "em",
            "ul",
            "ol",
            "li",
            "img",
            "table",
            "thead",
            "tbody",
            "tr",
            "th",
            "td",
            "style",
          ],
          ALLOWED_ATTR: ["href", "target", "rel", "class", "src", "alt", "style"],
        });
        const doc = `<!DOCTYPE html><html><head><meta charset="utf-8"><base target="_blank"></head><body>${sanitized}</body></html>`;
        return html`<iframe
          class="iframe-wrap"
          srcdoc=${doc}
          sandbox="allow-scripts"
          title="HTML preview"
        ></iframe>`;
      }
      case "diagram":
        return html`<div class="body">
          <pre><code>${this.content}</code></pre>
        </div>`;
      default:
        return html`<div class="body">
          <pre><code>${this.content}</code></pre>
        </div>`;
    }
  }

  override render() {
    return html`
      <div class="viewer" role="region" aria-label=${`${this._getTypeLabel()} artifact`}>
        <div class="toolbar">
          <span class="type-label">${this._getTypeLabel()}</span>
          <button
            type="button"
            class="copy-btn"
            @click=${this._onCopy}
            aria-label="Copy to clipboard"
          >
            ${icons.copy} Copy
          </button>
        </div>
        ${this._renderBody()}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-artifact-viewer": ScArtifactViewer;
  }
}
