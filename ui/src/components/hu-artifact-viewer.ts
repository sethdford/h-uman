import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import DOMPurify from "dompurify";
import "./hu-code-block.js";
import "./hu-toast.js";
import { ScToast } from "./hu-toast.js";
import { renderMarkdown } from "../lib/markdown.js";
import { icons } from "../icons.js";

export type ArtifactViewerType = "code" | "document" | "html" | "diagram";

interface DiffLine {
  type: "added" | "removed" | "unchanged";
  content: string;
}

function computeLineDiff(oldText: string, newText: string): DiffLine[] {
  const oldLines = oldText.split("\n");
  const newLines = newText.split("\n");
  const result: DiffLine[] = [];

  const m = oldLines.length;
  const n = newLines.length;
  const dp: number[][] = Array.from({ length: m + 1 }, () => Array(n + 1).fill(0));
  for (let i = 1; i <= m; i++) {
    for (let j = 1; j <= n; j++) {
      dp[i][j] =
        oldLines[i - 1] === newLines[j - 1]
          ? dp[i - 1][j - 1] + 1
          : Math.max(dp[i - 1][j], dp[i][j - 1]);
    }
  }

  let i = m,
    j = n;
  const stack: DiffLine[] = [];
  while (i > 0 || j > 0) {
    if (i > 0 && j > 0 && oldLines[i - 1] === newLines[j - 1]) {
      stack.push({ type: "unchanged", content: oldLines[i - 1] });
      i--;
      j--;
    } else if (j > 0 && (i === 0 || dp[i][j - 1] >= dp[i - 1][j])) {
      stack.push({ type: "added", content: newLines[j - 1] });
      j--;
    } else {
      stack.push({ type: "removed", content: oldLines[i - 1] });
      i--;
    }
  }
  stack.reverse();
  result.push(...stack);
  return result;
}

const BRIDGE_SCRIPT = `
<script>
(function() {
  var _store = {};
  try {
    var saved = null;
    window.addEventListener('message', function(e) {
      if (e.data && e.data.type === 'hu-storage-init') {
        _store = e.data.data || {};
      }
    });
    var proxy = {
      getItem: function(key) { return _store[key] !== undefined ? _store[key] : null; },
      setItem: function(key, value) {
        _store[key] = String(value);
        parent.postMessage({ type: 'hu-storage-set', key: key, value: String(value) }, '*');
      },
      removeItem: function(key) {
        delete _store[key];
        parent.postMessage({ type: 'hu-storage-remove', key: key }, '*');
      },
      clear: function() {
        _store = {};
        parent.postMessage({ type: 'hu-storage-clear' }, '*');
      },
      get length() { return Object.keys(_store).length; },
      key: function(n) { return Object.keys(_store)[n] || null; }
    };
    Object.defineProperty(window, 'localStorage', { value: proxy, writable: false, configurable: true });
  } catch(e) {}
  window.__hu = {
    sendToHost: function(type, data) {
      parent.postMessage({ type: 'hu-artifact-msg', msgType: type, data: data }, '*');
    }
  };
  parent.postMessage({ type: 'hu-artifact-ready' }, '*');
})();
<\/script>`;

@customElement("hu-artifact-viewer")
export class ScArtifactViewer extends LitElement {
  @property({ type: String }) type: ArtifactViewerType = "code";
  @property({ type: String }) content = "";
  @property({ type: String }) language = "";
  @property({ type: Boolean }) diffMode = false;
  @property({ type: String }) previousContent = "";
  @property({ type: String }) artifactId = "";

  private _storageData: Record<string, string> = {};
  private _messageHandler = (e: MessageEvent) => this._onIframeMessage(e);

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
    .diff-toggle {
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
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }
    .diff-toggle:hover {
      color: var(--hu-text);
      border-color: var(--hu-border);
    }
    .diff-toggle.active {
      color: var(--hu-accent-text, var(--hu-accent));
      border-color: color-mix(in srgb, var(--hu-accent) 40%, transparent);
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
    }
    .diff-toggle:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .diff-toggle svg {
      width: 0.875rem;
      height: 0.875rem;
    }
    .toolbar-actions {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }
    .diff-view {
      flex: 1;
      overflow: auto;
      padding: 0;
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      line-height: 1.6;
    }
    .diff-line {
      display: flex;
      padding: 0 var(--hu-space-md);
      min-height: 1.5em;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .diff-line.added {
      background: color-mix(in srgb, var(--hu-success) 12%, transparent);
      color: var(--hu-text);
    }
    .diff-line.removed {
      background: color-mix(in srgb, var(--hu-error) 12%, transparent);
      color: var(--hu-text);
      text-decoration: line-through;
      opacity: 0.7;
    }
    .diff-line.unchanged {
      color: var(--hu-text-muted);
    }
    .diff-gutter {
      flex-shrink: 0;
      width: 1.5rem;
      text-align: center;
      color: var(--hu-text-muted);
      user-select: none;
      font-size: var(--hu-text-xs);
      line-height: 1.6;
      opacity: 0.6;
    }
    .diff-content {
      flex: 1;
      padding-inline-start: var(--hu-space-xs);
    }
    @media (prefers-reduced-motion: reduce) {
      .copy-btn,
      .diff-toggle {
        transition: none;
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    window.addEventListener("message", this._messageHandler);
    this._loadStorage();
  }

  override disconnectedCallback(): void {
    window.removeEventListener("message", this._messageHandler);
    super.disconnectedCallback();
  }

  private _storageKey(): string {
    return `hu-artifact-storage-${this.artifactId || "default"}`;
  }

  private _loadStorage(): void {
    try {
      const raw = window.localStorage.getItem(this._storageKey());
      this._storageData = raw ? JSON.parse(raw) : {};
    } catch {
      this._storageData = {};
    }
  }

  private _saveStorage(): void {
    try {
      window.localStorage.setItem(this._storageKey(), JSON.stringify(this._storageData));
    } catch {
      /* quota exceeded or unavailable */
    }
  }

  private _onIframeMessage(e: MessageEvent): void {
    if (!e.data || typeof e.data.type !== "string") return;
    switch (e.data.type) {
      case "hu-artifact-ready": {
        const iframe = this.shadowRoot?.querySelector("iframe");
        iframe?.contentWindow?.postMessage(
          { type: "hu-storage-init", data: this._storageData },
          "*",
        );
        break;
      }
      case "hu-storage-set":
        this._storageData[e.data.key] = e.data.value;
        this._saveStorage();
        break;
      case "hu-storage-remove":
        delete this._storageData[e.data.key];
        this._saveStorage();
        break;
      case "hu-storage-clear":
        this._storageData = {};
        this._saveStorage();
        break;
      case "hu-artifact-msg":
        this.dispatchEvent(
          new CustomEvent("hu-artifact-message", {
            bubbles: true,
            composed: true,
            detail: { msgType: e.data.msgType, data: e.data.data },
          }),
        );
        break;
    }
  }

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
            "script",
            "canvas",
            "svg",
            "path",
            "circle",
            "rect",
            "line",
            "g",
            "defs",
            "use",
            "input",
            "button",
            "select",
            "option",
            "form",
            "label",
            "textarea",
          ],
          ALLOWED_ATTR: [
            "href",
            "target",
            "rel",
            "class",
            "src",
            "alt",
            "style",
            "id",
            "type",
            "value",
            "placeholder",
            "width",
            "height",
            "viewBox",
            "fill",
            "stroke",
            "d",
            "cx",
            "cy",
            "r",
            "x",
            "y",
            "onclick",
            "oninput",
            "onchange",
          ],
          ADD_TAGS: ["script"],
          FORCE_BODY: true,
        });
        const doc = `<!DOCTYPE html><html><head><meta charset="utf-8"><base target="_blank">${BRIDGE_SCRIPT}</head><body>${sanitized}</body></html>`;
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

  private _toggleDiff(): void {
    this.diffMode = !this.diffMode;
  }

  private _renderDiff() {
    const lines = computeLineDiff(this.previousContent, this.content);
    return html`
      <div class="diff-view" role="region" aria-label="Diff view">
        ${lines.map(
          (line) => html`
            <div class="diff-line ${line.type}">
              <span class="diff-gutter"
                >${line.type === "added" ? "+" : line.type === "removed" ? "−" : " "}</span
              >
              <span class="diff-content">${line.content || " "}</span>
            </div>
          `,
        )}
      </div>
    `;
  }

  override render() {
    const hasPrevious = !!this.previousContent;
    return html`
      <div class="viewer" role="region" aria-label=${`${this._getTypeLabel()} artifact`}>
        <div class="toolbar">
          <span class="type-label">${this._getTypeLabel()}</span>
          <div class="toolbar-actions">
            ${
              hasPrevious
                ? html`<button
                    type="button"
                    class="diff-toggle ${this.diffMode ? "active" : ""}"
                    @click=${this._toggleDiff}
                    aria-label=${this.diffMode ? "Hide diff" : "Show diff"}
                    aria-pressed=${this.diffMode}
                  >
                    ${icons.code} Diff
                  </button>`
                : nothing
            }
            <button
              type="button"
              class="copy-btn"
              @click=${this._onCopy}
              aria-label="Copy to clipboard"
            >
              ${icons.copy} Copy
            </button>
          </div>
        </div>
        ${this.diffMode && hasPrevious ? this._renderDiff() : this._renderBody()}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-artifact-viewer": ScArtifactViewer;
  }
}
