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
  "swift",
  "zig",
]);

const LANG_ALIASES: Record<string, string> = { shell: "bash" };

type HighlighterCore = {
  codeToHtml: (code: string, opts: { lang: string; theme: string }) => string;
};
let _highlighterPromise: Promise<HighlighterCore> | null = null;

async function getHighlighter(): Promise<HighlighterCore> {
  if (_highlighterPromise) return _highlighterPromise;
  _highlighterPromise = (async () => {
    const { createHighlighterCore } = await import("shiki/core");
    const { createOnigurumaEngine } = await import("shiki/engine/oniguruma");
    return createHighlighterCore({
      themes: [
        import("@shikijs/themes/github-dark-default"),
        import("@shikijs/themes/github-light-default"),
      ],
      langs: [
        import("@shikijs/langs/javascript"),
        import("@shikijs/langs/typescript"),
        import("@shikijs/langs/python"),
        import("@shikijs/langs/bash"),
        import("@shikijs/langs/json"),
        import("@shikijs/langs/html"),
        import("@shikijs/langs/css"),
        import("@shikijs/langs/c"),
        import("@shikijs/langs/rust"),
        import("@shikijs/langs/go"),
        import("@shikijs/langs/sql"),
        import("@shikijs/langs/yaml"),
        import("@shikijs/langs/markdown"),
        import("@shikijs/langs/jsx"),
        import("@shikijs/langs/tsx"),
        import("@shikijs/langs/swift"),
        import("@shikijs/langs/zig"),
      ],
      engine: createOnigurumaEngine(import("shiki/wasm")),
    });
  })();
  return _highlighterPromise;
}

@customElement("hu-code-block")
export class ScCodeBlock extends LitElement {
  @property({ type: String }) code = "";
  @property({ type: String }) language = "";
  @property({ type: Object }) onCopy?: (code: string) => void;

  @state() private _highlighted = "";
  @state() private _copied = false;
  @state() private _shikiReady = false;
  @state() private _shikiFailed = false;
  @state() private _darkScheme = true;
  @state() private _expanded = false;
  private _copyTimeout = 0;
  private _highlightGeneration = 0;
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
      border: 1px solid color-mix(in srgb, var(--hu-color-white) 8%, transparent);
      border-radius: var(--hu-radius-full);
      cursor: pointer;
      opacity: 0;
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
      z-index: 1;
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
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
    }

    .content.loading {
      opacity: 0.5;
    }

    .shiki-error {
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      padding: var(--hu-space-2xs) var(--hu-space-md);
      border-top: 1px solid var(--hu-border-subtle);
    }

    .code-wrapper {
      display: flex;
      overflow: hidden;
    }

    .code-wrapper.truncated {
      max-height: calc(var(--hu-text-sm) * 1.5 * 30 + var(--hu-space-md) * 2);
      position: relative;
    }

    .truncated-fade {
      position: absolute;
      bottom: 0;
      left: 0;
      right: 0;
      height: var(--hu-space-3xl, 3rem);
      background: linear-gradient(transparent, var(--hu-bg-inset));
      pointer-events: none;
      z-index: 1;
    }

    .gutter {
      flex-shrink: 0;
      padding: var(--hu-space-md) 0;
      border-right: 1px solid var(--hu-border-subtle);
      user-select: none;
      -webkit-user-select: none;
      text-align: right;
      color: var(--hu-text-muted);
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-2xs);
    }

    .line-number {
      padding: 0 var(--hu-space-sm);
      height: calc(var(--hu-text-sm) * 1.5);
      line-height: calc(var(--hu-text-sm) * 1.5);
    }

    .content {
      flex: 1;
      min-width: 0;
      padding: var(--hu-space-md);
      overflow-x: auto;
    }

    .content pre {
      margin: 0;
      white-space: pre;
      line-height: 1.5;
      background: transparent !important;
    }

    .content :global(code) {
      font-family: var(--hu-font-mono);
      font-size: inherit;
    }

    .show-more-btn {
      display: block;
      width: 100%;
      padding: var(--hu-space-xs);
      background: color-mix(in srgb, var(--hu-border) 10%, var(--hu-bg-inset));
      border: none;
      border-top: 1px solid var(--hu-border-subtle);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      cursor: pointer;
      text-align: center;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .show-more-btn:hover {
      color: var(--hu-text);
    }

    .show-more-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: -2px;
    }

    @media (prefers-reduced-transparency: reduce) {
      .copy-btn {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-bg-elevated);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .copy-btn,
      .show-more-btn {
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
      this._expanded = false;
    }
  }

  private async _highlight(): Promise<void> {
    const gen = ++this._highlightGeneration;
    let lang = this.language.toLowerCase().trim();
    lang = LANG_ALIASES[lang] ?? lang;
    const supported = lang && SHIKI_LANGS.has(lang);
    if (!supported) {
      if (gen !== this._highlightGeneration) return;
      this._highlighted = "";
      this._shikiReady = true;
      this._shikiFailed = false;
      return;
    }
    try {
      const highlighter = await getHighlighter();
      if (gen !== this._highlightGeneration) return;
      const theme = this._darkScheme ? "github-dark-default" : "github-light-default";
      const result = highlighter.codeToHtml(this.code, { lang, theme });
      if (gen !== this._highlightGeneration) return;
      this._highlighted = result;
      this._shikiFailed = false;
    } catch {
      if (gen !== this._highlightGeneration) return;
      this._highlighted = "";
      this._shikiFailed = true;
    }
    if (gen !== this._highlightGeneration) return;
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

  private _toggleExpand(): void {
    this._expanded = !this._expanded;
  }

  override render() {
    const langLabel = this.language ? this.language.toLowerCase() : "plain";
    const showHighlighted = this._shikiReady && this._highlighted;
    const lineCount = this.code ? this.code.split("\n").length : 0;
    const canTruncate = lineCount > 30;
    const shouldTruncate = canTruncate && !this._expanded;
    const hiddenCount = lineCount - 30;

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
        <div class="code-wrapper ${shouldTruncate ? "truncated" : ""}">
          <div class="gutter" aria-hidden="true">
            ${Array.from(
              { length: lineCount },
              (_, i) => html`<div class="line-number">${i + 1}</div>`,
            )}
          </div>
          <div class="content ${!this._shikiReady && this.language ? "loading" : ""}">
            ${
              showHighlighted
                ? html`${unsafeHTML(DOMPurify.sanitize(this._highlighted))}`
                : html`<pre><code>${this.code}</code></pre>`
            }
          </div>
          ${shouldTruncate ? html`<div class="truncated-fade"></div>` : ""}
        </div>
        ${
          this._shikiFailed
            ? html`<div class="shiki-error">Syntax highlighting unavailable</div>`
            : ""
        }
        ${
          canTruncate
            ? html`
                <button
                  type="button"
                  class="show-more-btn"
                  @click=${this._toggleExpand}
                  aria-expanded=${this._expanded}
                >
                  ${this._expanded ? "Show less" : `Show more (${hiddenCount} lines)`}
                </button>
              `
            : ""
        }
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-code-block": ScCodeBlock;
  }
}
