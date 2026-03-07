import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import "./sc-toast.js";
import { ScToast } from "./sc-toast.js";

const SHIKI_LANGS = new Set([
  "javascript",
  "typescript",
  "python",
  "bash",
  "json",
  "html",
  "css",
  "c",
  "rust",
  "go",
  "sql",
  "yaml",
  "markdown",
  "shell",
  "jsx",
  "tsx",
  "java",
  "ruby",
  "php",
  "swift",
  "kotlin",
  "zig",
]);

@customElement("sc-code-block")
export class ScCodeBlock extends LitElement {
  @property({ type: String }) code = "";
  @property({ type: String }) language = "";
  @property({ type: Object }) onCopy?: (code: string) => void;

  @state() private _highlighted = "";
  @state() private _copied = false;
  @state() private _shikiReady = false;

  static override styles = css`
    :host {
      display: block;
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      background: var(--sc-bg-inset);
      border-radius: var(--sc-radius-md);
      overflow: hidden;
      margin: var(--sc-space-sm) 0;
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: color-mix(in srgb, var(--sc-border) 20%, var(--sc-bg-inset));
      border-bottom: 1px solid var(--sc-border-subtle);
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .lang-label {
      text-transform: lowercase;
    }

    .copy-btn {
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      background: transparent;
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-sm);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .copy-btn:hover {
      color: var(--sc-text);
      border-color: var(--sc-border);
    }

    .copy-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .copy-btn.copied {
      color: var(--sc-success);
      border-color: var(--sc-success);
    }

    .content {
      padding: var(--sc-space-md);
      overflow-x: auto;
    }

    .content pre {
      margin: 0;
      white-space: pre;
    }

    .content :global(code) {
      font-family: var(--sc-font-mono);
      font-size: inherit;
    }

    @media (prefers-reduced-motion: reduce) {
      .copy-btn {
        transition: none;
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    this._highlight();
  }

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("code") || changed.has("language")) {
      this._highlight();
    }
  }

  private async _highlight(): Promise<void> {
    const lang = this.language.toLowerCase().trim();
    const supported = lang && SHIKI_LANGS.has(lang);
    if (!supported) {
      this._highlighted = "";
      this._shikiReady = true;
      return;
    }
    try {
      const { codeToHtml } = await import("shiki");
      const html = await codeToHtml(this.code, {
        lang,
        theme: "github-dark-default",
      });
      this._highlighted = html;
    } catch {
      this._highlighted = "";
    }
    this._shikiReady = true;
  }

  private async _onCopy(): Promise<void> {
    if (this.onCopy) {
      this.onCopy(this.code);
      this._copied = true;
      setTimeout(() => {
        this._copied = false;
        this.requestUpdate();
      }, 2000);
      return;
    }
    try {
      await navigator.clipboard.writeText(this.code);
      ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      this._copied = true;
      setTimeout(() => {
        this._copied = false;
        this.requestUpdate();
      }, 2000);
    } catch {
      ScToast.show({ message: "Failed to copy", variant: "error" });
    }
  }

  override render() {
    const langLabel = this.language ? this.language.toLowerCase() : "plain";
    const showHighlighted = this._shikiReady && this._highlighted;
    return html`
      <div
        class="code-block"
        role="region"
        aria-label=${`Code block${this.language ? ` (${langLabel})` : ""}`}
      >
        <div class="header">
          <span class="lang-label">${langLabel}</span>
          <button
            type="button"
            class="copy-btn ${this._copied ? "copied" : ""}"
            aria-label=${this._copied ? "Copied" : "Copy code"}
            @click=${this._onCopy}
          >
            ${this._copied ? "Copied!" : "Copy"}
          </button>
        </div>
        <div class="content">
          ${showHighlighted
            ? html`${unsafeHTML(this._highlighted)}`
            : html`<pre><code>${this.code}</code></pre>`}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-code-block": ScCodeBlock;
  }
}
