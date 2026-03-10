import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

type JsonValue = string | number | boolean | null | { [key: string]: JsonValue } | JsonValue[];

function isObject(v: unknown): v is Record<string, JsonValue> {
  return v !== null && typeof v === "object" && !Array.isArray(v);
}

function isArray(v: unknown): v is JsonValue[] {
  return Array.isArray(v);
}

function isExpandable(v: unknown): v is Record<string, JsonValue> | JsonValue[] {
  return isObject(v) || isArray(v);
}

@customElement("hu-json-viewer")
export class ScJsonViewer extends LitElement {
  @property() data: unknown = undefined;

  @property({ type: Number, attribute: "expanded-depth" }) expandedDepth = 2;

  @property({ type: String, attribute: "root-label" }) rootLabel = "";

  @state() private _expanded = new Set<string>();

  @state() private _focusedPath: string | null = null;

  @state() private _hoveredPath: string | null = null;

  override connectedCallback(): void {
    super.connectedCallback();
    this._syncExpandedFromDepth();
  }

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("data") || changed.has("expandedDepth")) {
      this._syncExpandedFromDepth();
    }
  }

  private _syncExpandedFromDepth(): void {
    const next = new Set<string>();
    const visit = (path: string, depth: number, val: unknown): void => {
      if (!isExpandable(val)) return;
      if (depth < this.expandedDepth) {
        next.add(path);
      }
      if (isObject(val)) {
        for (const k of Object.keys(val)) {
          visit(`${path}.${k}`, depth + 1, val[k]);
        }
      } else if (isArray(val)) {
        val.forEach((v, i) => {
          visit(`${path}[${i}]`, depth + 1, v);
        });
      }
    };
    visit("root", 0, this.data);
    this._expanded = next;
  }

  private _isExpanded(path: string): boolean {
    return this._expanded.has(path);
  }

  private _toggle(path: string): void {
    const next = new Set(this._expanded);
    if (next.has(path)) {
      next.delete(path);
    } else {
      next.add(path);
    }
    this._expanded = next;
  }

  private _getSummary(val: Record<string, JsonValue> | JsonValue[]): string {
    if (isArray(val)) {
      const n = val.length;
      return n === 1 ? "[1 item]" : `[${n} items]`;
    }
    const n = Object.keys(val).length;
    return n === 1 ? "{1 key}" : `{${n} keys}`;
  }

  private _getCopyText(_path: string, val: unknown): string {
    if (isExpandable(val)) {
      return JSON.stringify(val);
    }
    return JSON.stringify(val);
  }

  private async _copy(path: string, val: unknown): Promise<void> {
    const text = this._getCopyText(path, val);
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      /* ignore */
    }
  }

  private _renderPrimitive(val: string | number | boolean | null): ReturnType<typeof html> {
    if (typeof val === "string") {
      return html`<span class="val-string">${JSON.stringify(val)}</span>`;
    }
    if (typeof val === "number") {
      return html`<span class="val-number">${JSON.stringify(val)}</span>`;
    }
    if (typeof val === "boolean") {
      return html`<span class="val-bool">${JSON.stringify(val)}</span>`;
    }
    return html`<span class="val-null">null</span>`;
  }

  private _renderNode(
    path: string,
    key: string | number | null,
    val: unknown,
    depth: number,
  ): ReturnType<typeof html> {
    const isExpandableVal = isExpandable(val);
    const expanded = this._isExpanded(path);
    const isHovered = this._hoveredPath === path;
    const isFocused = this._focusedPath === path;

    if (!isExpandableVal) {
      return html`
        <div
          class="row row-primitive"
          role="treeitem"
          tabindex=${isFocused ? 0 : -1}
          aria-label=${key !== null
            ? `${String(key)}: ${JSON.stringify(val)}`
            : JSON.stringify(val)}
          data-path=${path}
          @mouseenter=${() => {
            this._hoveredPath = path;
          }}
          @mouseleave=${() => {
            this._hoveredPath = null;
          }}
          @focus=${() => {
            this._focusedPath = path;
          }}
          @blur=${() => {
            if (this._focusedPath === path) this._focusedPath = null;
          }}
          @keydown=${(e: KeyboardEvent) => this._onKeyDown(e, path, val)}
          style="padding-left: calc(var(--hu-space-md) * ${depth})"
        >
          <span class="indent"></span>
          ${key !== null
            ? html`<span class="key">${JSON.stringify(String(key))}:</span> `
            : nothing}
          ${this._renderPrimitive(val as string | number | boolean | null)}
          ${isHovered
            ? html`
                <button
                  type="button"
                  class="copy-btn"
                  aria-label="Copy"
                  @click=${(e: Event) => {
                    e.stopPropagation();
                    this._copy(path, val);
                  }}
                >
                  ${icons.copy}
                </button>
              `
            : nothing}
        </div>
      `;
    }

    const summary = this._getSummary(val);
    const label =
      key !== null
        ? `${String(key)}: ${isArray(val) ? "Array" : "Object"} ${summary}`
        : isArray(val)
          ? `Array ${summary}`
          : `Object ${summary}`;

    return html`
      <div
        class="row row-expandable"
        role="treeitem"
        aria-expanded=${expanded}
        aria-label=${label}
        tabindex=${isFocused ? 0 : -1}
        data-path=${path}
        @mouseenter=${() => {
          this._hoveredPath = path;
        }}
        @mouseleave=${() => {
          this._hoveredPath = null;
        }}
        @focus=${() => {
          this._focusedPath = path;
        }}
        @blur=${() => {
          if (this._focusedPath === path) this._focusedPath = null;
        }}
        @keydown=${(e: KeyboardEvent) => this._onKeyDown(e, path, val)}
        style="padding-left: calc(var(--hu-space-md) * ${depth})"
      >
        <button
          type="button"
          class="toggle"
          aria-label=${expanded ? "Collapse" : "Expand"}
          @click=${(e: Event) => {
            e.stopPropagation();
            this._toggle(path);
          }}
        >
          <span class="arrow ${expanded ? "expanded" : ""}">${icons["caret-right"]}</span>
        </button>
        ${key !== null ? html`<span class="key">${JSON.stringify(String(key))}:</span> ` : nothing}
        <span class="bracket">${isArray(val) ? "[" : "{"}</span>
        ${expanded ? nothing : html`<span class="summary">${summary}</span>`}
        ${expanded ? nothing : html`<span class="bracket">${isArray(val) ? "]" : "}"}</span>`}
        ${isHovered
          ? html`
              <button
                type="button"
                class="copy-btn"
                aria-label="Copy"
                @click=${(e: Event) => {
                  e.stopPropagation();
                  this._copy(path, val);
                }}
              >
                ${icons.copy}
              </button>
            `
          : nothing}
      </div>
      ${expanded
        ? html`
            <div role="group" class="children">
              ${isObject(val)
                ? Object.entries(val).map(([k, v]) =>
                    this._renderNode(`${path}.${k}`, k, v, depth + 1),
                  )
                : (val as JsonValue[]).map((v, i) =>
                    this._renderNode(`${path}[${i}]`, String(i), v, depth + 1),
                  )}
            </div>
          `
        : nothing}
    `;
  }

  private _onKeyDown(e: KeyboardEvent, path: string, val: unknown): void {
    const tree = this.shadowRoot?.querySelector('[role="tree"]');
    const items = tree?.querySelectorAll('[role="treeitem"]') ?? [];
    const arr = Array.from(items) as HTMLElement[];
    const idx = arr.findIndex((el) => el.getAttribute("data-path") === path);
    if (idx < 0) return;

    switch (e.key) {
      case "Enter":
      case " ":
        e.preventDefault();
        if (isExpandable(val)) {
          this._toggle(path);
        }
        break;
      case "ArrowLeft":
        e.preventDefault();
        if (isExpandable(val) && this._isExpanded(path)) {
          this._toggle(path);
        } else {
          const parent = this._parentPath(path);
          if (parent) {
            (arr.find((el) => el.getAttribute("data-path") === parent) as HTMLElement)?.focus();
          }
        }
        break;
      case "ArrowRight":
        e.preventDefault();
        if (isExpandable(val) && !this._isExpanded(path)) {
          this._toggle(path);
        } else if (isExpandable(val) && this._isExpanded(path) && arr[idx + 1]) {
          arr[idx + 1].focus();
        }
        break;
      case "ArrowUp":
        e.preventDefault();
        if (idx > 0) arr[idx - 1].focus();
        break;
      case "ArrowDown":
        e.preventDefault();
        if (idx < arr.length - 1) arr[idx + 1].focus();
        break;
    }
  }

  private _parentPath(path: string): string | null {
    if (path === "root") return null;
    const parent = path.replace(/\.[^.[]+$|\[\d+\]$/, "");
    return parent || "root";
  }

  override render() {
    const d = this.data;
    const label = this.rootLabel || "JSON";

    if (d === undefined) {
      return html`<div class="tree" role="tree" aria-label=${label}></div>`;
    }

    if (!isExpandable(d)) {
      return html`
        <div class="tree" role="tree" aria-label=${label}>
          ${this._renderNode("root", null, d, 0)}
        </div>
      `;
    }

    return html`
      <div class="tree" role="tree" aria-label=${label}>
        ${this._renderNode("root", this.rootLabel || null, d, 0)}
      </div>
    `;
  }

  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }

    .tree {
      outline: none;
    }

    .row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      min-height: 1.5em;
      cursor: default;
      position: relative;
    }

    .row:hover {
      background: var(--hu-hover-overlay);
    }

    .row:focus-within {
      background: var(--hu-hover-overlay);
    }

    .row-primitive .key,
    .row-expandable .key {
      color: var(--hu-text);
    }

    .val-string {
      color: var(--hu-accent);
    }

    .val-number {
      color: var(--hu-warning);
    }

    .val-bool {
      color: var(--hu-info);
    }

    .val-null {
      color: var(--hu-text-muted);
    }

    .toggle {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 0;
      margin: 0;
      background: none;
      border: none;
      cursor: pointer;
      color: var(--hu-text-muted);
    }

    .toggle:hover {
      color: var(--hu-text);
    }

    .toggle:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .arrow {
      display: inline-flex;
      width: 1em;
      height: 1em;
      flex-shrink: 0;
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .arrow.expanded {
      transform: rotate(90deg);
    }

    .indent {
      display: inline-block;
      width: 1em;
      flex-shrink: 0;
    }

    .summary {
      color: var(--hu-text-muted);
    }

    .bracket {
      color: var(--hu-text-muted);
    }

    .copy-btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-2xs);
      margin-left: auto;
      background: none;
      border: none;
      cursor: pointer;
      color: var(--hu-text-muted);
      border-radius: var(--hu-radius-sm);
    }

    .copy-btn:hover {
      color: var(--hu-accent);
    }

    .copy-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .copy-btn svg {
      width: 14px;
      height: 14px;
    }

    .children {
      display: block;
    }

    [role="treeitem"]:focus {
      outline: none;
    }

    [role="treeitem"]:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    @media (prefers-reduced-motion: reduce) {
      .arrow {
        transition: none;
      }
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-json-viewer": ScJsonViewer;
  }
}
