import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import DOMPurify from "dompurify";
import "./hu-toast.js";
import { ScToast } from "./hu-toast.js";

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

@customElement("hu-code-block")
export class ScCodeBlock extends LitElement {
  @property({ type: String }) code = "";
  @property({ type: String }) language = "";
  @property({ type: Object }) onCopy?: (code: string) => void;

  @state() private _highlighted = "";
  @state() private _copied = false;
  @state() private _shikiReady = false;
  @state() private _darkScheme = true;
  private _copyTimeout = 0;
  private _mediaQuery: MediaQueryList | null = null;
  private _mediaHandler: (() => void) | null = null;
  private _themeObserver: MutationObserver | null = null;

  static override styles = css`
    :host {
      display: block;
      position: relative;
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-md);
      overflow: hidden;
      margin: var(--hu-space-sm) 0;
    }

    .header {
      display: flex;
      align-items: center;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-border) 20%, var(--hu-bg-inset));
      border-bottom: 1px solid var(--hu-border-subtle);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .lang-label {
      text-transform: lowercase;
    }

    .copy-btn {
      position: absolute;
      top: var(--hu-space-sm);
      right: var(--hu-space-sm);
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      background: color-mix(in srgb, var(--hu-bg-surface) 65%, transparent);
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px));
      border: 1px solid color-mix(in srgb, white 8%, transparent);
      border-radius: var(--hu-radius-full);
      cursor: pointer;
      opacity: 0;
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    :host:hover .copy-btn,
    .copy-btn:focus,
    .copy-btn.copied {
      opacity: 1;
    }

    .copy-btn:hover {
      color: var(--hu-text);
    }

    .copy-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .copy-btn.copied {
      color: var(--hu-success);
      border-color: var(--hu-success);
    }

    .content {
      padding: var(--hu-space-md);
      overflow-x: auto;
    }

    .content pre {
      margin: 0;
      white-space: pre;
    }

    .content :global(code) {
      font-family: var(--hu-font-mono);
      font-size: inherit;
    }

    @media (prefers-reduced-transparency: reduce) {
      .copy-btn {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-bg-elevated);
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .copy-btn {
        transition: none;
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    this._darkScheme = this._resolveScheme();
    this._highlight();
    this._mediaQuery = window.matchMedia("(prefers-color-scheme: dark)");
    this._mediaHandler = () => {
      this._darkScheme = this._resolveScheme();
      this._highlight();
    };
    this._mediaQuery?.addEventListener("change", this._mediaHandler);
    this._themeObserver = new MutationObserver(() => {
      this._darkScheme = this._resolveScheme();
      this._highlight();
    });
    this._themeObserver.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ["data-theme"],
    });
  }

  override disconnectedCallback(): void {
    clearTimeout(this._copyTimeout);
    if (this._mediaQuery && this._mediaHandler) {
      this._mediaQuery.removeEventListener("change", this._mediaHandler);
    }
    this._themeObserver?.disconnect();
    this._mediaQuery = null;
    this._mediaHandler = null;
    this._themeObserver = null;
    super.disconnectedCallback();
  }

  private _resolveScheme(): boolean {
    const explicit = document.documentElement.dataset.theme;
    if (explicit === "dark") return true;
    if (explicit === "light") return false;
    return window.matchMedia("(prefers-color-scheme: dark)").matches;
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
      const theme = this._darkScheme ? "github-dark-default" : "github-light-default";
      const html = await codeToHtml(this.code, {
        lang,
        theme,
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
      this._copyTimeout = window.setTimeout(() => {
        this._copied = false;
        this.requestUpdate();
      }, 2000);
      return;
    }
    try {
      await navigator.clipboard.writeText(this.code);
      ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      this._copied = true;
      this._copyTimeout = window.setTimeout(() => {
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
        </div>
        <button
          type="button"
          class="copy-btn ${this._copied ? "copied" : ""}"
          aria-label=${this._copied ? "Copied" : "Copy code"}
          @click=${this._onCopy}
        >
          ${this._copied ? "Copied!" : "Copy"}
        </button>
        <div class="content">
          ${showHighlighted
            ? html`${unsafeHTML(DOMPurify.sanitize(this._highlighted))}`
            : html`<pre><code>${this.code}</code></pre>`}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-code-block": ScCodeBlock;
  }
}
