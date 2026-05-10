import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { GatewayClient } from "../gateway.js";
import { icons } from "../icons.js";
import type { CanvasFormat } from "../components/hu-canvas.js";
import "../components/hu-button.js";
import "../components/hu-card.js";
import "../components/hu-empty-state.js";
import "../components/hu-canvas.js";
import "../components/hu-canvas-editor.js";

const VALID_FORMATS: CanvasFormat[] = [
  "html",
  "svg",
  "mockup",
  "react",
  "mermaid",
  "markdown",
  "code",
];

function resolveFormat(raw: unknown): CanvasFormat {
  if (typeof raw === "string" && VALID_FORMATS.includes(raw as CanvasFormat))
    return raw as CanvasFormat;
  return "html";
}

interface CanvasEntry {
  id: string;
  title: string;
  format: CanvasFormat;
  content: string;
  imports: Record<string, string>;
  language: string;
  versionSeq: number;
  status: "active" | "closed";
}

@customElement("hu-canvas-view")
export class CanvasView extends GatewayAwareLitElement {
  @state() private canvases: CanvasEntry[] = [];
  @state() private selectedId: string | null = null;
  @state() private _viewMode: "preview" | "code" | "split" = "preview";
  @state() private _loading = false;
  @state() private _error = "";
  @state() private _versionsOpen = false;
  @state() private _versions: Array<{ seq: number; created_at: number; summary: string }> = [];
  private _editTimer: ReturnType<typeof setTimeout> | null = null;

  private _onGateway = ((e: Event) => {
    const ev = e as CustomEvent<{ event: string; payload: Record<string, unknown> }>;
    if (ev.detail?.event !== "canvas") return;
    const p = ev.detail.payload ?? {};
    const action = String(p.action ?? "");
    const canvasId = typeof p.canvas_id === "string" ? p.canvas_id : "";
    if (!canvasId) return;

    if (action === "create") {
      const title = typeof p.title === "string" ? p.title : canvasId;
      const format = resolveFormat(p.format);
      const imports =
        p.imports && typeof p.imports === "object" ? (p.imports as Record<string, string>) : {};
      const language = typeof p.language === "string" ? p.language : "";
      const next = this.canvases.filter((c) => c.id !== canvasId);
      next.push({
        id: canvasId,
        title,
        format,
        content: "",
        imports,
        language,
        versionSeq: 0,
        status: "active",
      });
      this.canvases = next;
      this.selectedId = canvasId;
      return;
    }

    if (action === "update") {
      const content = typeof p.content === "string" ? p.content : "";
      const format = resolveFormat(p.format);
      const imports =
        p.imports && typeof p.imports === "object"
          ? (p.imports as Record<string, string>)
          : undefined;
      const language = typeof p.language === "string" ? p.language : undefined;
      const versionSeq = typeof p.version_seq === "number" ? p.version_seq : undefined;
      this.canvases = this.canvases.map((c) => {
        if (c.id !== canvasId) return c;
        return {
          ...c,
          content,
          format: p.format !== undefined ? format : c.format,
          ...(imports !== undefined ? { imports } : {}),
          ...(language !== undefined ? { language } : {}),
          ...(versionSeq !== undefined ? { versionSeq } : {}),
        };
      });
      if (!this.selectedId) this.selectedId = canvasId;
      return;
    }

    if (action === "clear") {
      this.canvases = this.canvases.map((c) => (c.id === canvasId ? { ...c, content: "" } : c));
      return;
    }

    if (action === "close") {
      this.canvases = this.canvases.filter((c) => c.id !== canvasId);
      if (this.selectedId === canvasId) {
        this.selectedId = this.canvases[0]?.id ?? null;
      }
    }
  }) as EventListener;

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-canvas;
        display: block;
        width: 100%;
        min-width: 0;
        color: var(--hu-text);
        max-width: 90rem;
        contain: layout style;
        container-type: inline-size;
        padding: var(--hu-space-adaptive-page-y) var(--hu-space-adaptive-page-x);
      }

      .layout {
        display: block;
      }

      @media (min-width: 1240px) /* --hu-breakpoint-wide */ {
        .layout.has-detail {
          display: grid;
          grid-template-columns: minmax(min(14rem, 100%), 1fr) minmax(0, 3fr);
          gap: var(--hu-space-lg);
          align-items: start;
        }
      }

      .list-col h1 {
        font-size: var(--hu-text-xl);
        margin: 0 0 var(--hu-space-md);
      }

      .list {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-sm);
      }

      .row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: var(--hu-space-sm);
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-radius: var(--hu-radius-md);
        border: 1px solid var(--hu-border-subtle);
        background: var(--hu-surface-container);
        cursor: pointer;
        text-align: left;
        width: 100%;
        font-family: var(--hu-font);
        color: var(--hu-text);
        transition:
          background var(--hu-duration-fast) var(--hu-ease-out),
          border-color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .row:hover {
        background: var(--hu-surface-container-high);
      }

      .row:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .row:active {
        transform: scale(0.99);
      }

      .row.selected {
        border-color: var(--hu-accent);
        box-shadow: 0 0 0 1px color-mix(in srgb, var(--hu-accent) 35%, transparent);
      }

      .row-title {
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-semibold);
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }

      .row-meta {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-secondary);
      }

      .detail {
        border-radius: var(--hu-radius-xl);
        border: 1px solid var(--hu-border-subtle);
        background: var(--hu-surface-container);
        box-shadow: var(--hu-shadow-card);
        min-height: 20rem;
        display: flex;
        flex-direction: column;
        overflow: hidden;
      }

      .detail-header {
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-bottom: 1px solid var(--hu-border-subtle);
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: var(--hu-space-md);
      }

      .detail-header h2 {
        margin: 0;
        font-size: var(--hu-text-lg);
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }

      .detail-controls {
        display: flex;
        align-items: center;
        gap: var(--hu-space-xs);
        flex-shrink: 0;
      }

      .format-selector {
        display: inline-flex;
        flex-wrap: wrap;
        gap: var(--hu-space-2xs);
        border-radius: var(--hu-radius-md);
        overflow: hidden;
      }

      .format-selector button {
        padding: var(--hu-space-2xs) var(--hu-space-sm);
        border: 1px solid var(--hu-border-subtle);
        border-radius: var(--hu-radius-sm);
        background: transparent;
        color: var(--hu-text-secondary);
        font-family: var(--hu-font);
        font-size: var(--hu-text-xs);
        cursor: pointer;
        transition:
          background var(--hu-duration-fast) var(--hu-ease-out),
          border-color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .format-selector button:hover {
        background: var(--hu-hover-overlay);
      }

      .format-selector button:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .format-selector button.active {
        background: color-mix(in srgb, var(--hu-accent) 15%, transparent);
        border-color: var(--hu-accent);
        color: var(--hu-accent);
        font-weight: var(--hu-weight-semibold);
      }

      .mode-toggle {
        display: inline-flex;
        border-radius: var(--hu-radius-md);
        border: 1px solid var(--hu-border-subtle);
        overflow: hidden;
      }

      .mode-toggle button {
        padding: var(--hu-space-xs) var(--hu-space-sm);
        border: none;
        background: transparent;
        color: var(--hu-text-secondary);
        font-family: var(--hu-font);
        font-size: var(--hu-text-xs);
        cursor: pointer;
        transition: background var(--hu-duration-fast) var(--hu-ease-out);
      }

      .mode-toggle button:hover {
        background: var(--hu-hover-overlay);
      }

      .mode-toggle button:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .mode-toggle button.active {
        background: color-mix(in srgb, var(--hu-accent) 15%, transparent);
        color: var(--hu-accent);
        font-weight: var(--hu-weight-semibold);
      }

      .version-info {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-secondary);
        padding: 0 var(--hu-space-xs);
      }

      .version-btn {
        display: inline-flex;
        align-items: center;
        gap: var(--hu-space-2xs);
        cursor: pointer;
        transition: color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .version-btn:hover {
        color: var(--hu-accent);
      }

      .version-panel {
        position: absolute;
        top: 0;
        right: 0;
        bottom: 0;
        width: 14rem;
        background: var(--hu-surface-container-high);
        border-left: 1px solid var(--hu-border);
        overflow-y: auto;
        z-index: 10;
        animation: hu-slide-in-right var(--hu-duration-normal) var(--hu-ease-out);
      }

      @keyframes hu-slide-in-right {
        from {
          transform: translateX(100%);
        }
        to {
          transform: translateX(0);
        }
      }

      .version-panel-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: var(--hu-space-sm) var(--hu-space-md);
        border-bottom: 1px solid var(--hu-border-subtle);
        font-weight: var(--hu-weight-semibold);
        font-size: var(--hu-text-sm);
      }

      .version-panel-close {
        background: none;
        border: none;
        color: var(--hu-text-secondary);
        cursor: pointer;
        padding: var(--hu-space-2xs);
        border-radius: var(--hu-radius-sm);
        transition:
          color var(--hu-duration-fast) var(--hu-ease-out),
          background var(--hu-duration-fast) var(--hu-ease-out);
      }

      .version-panel-close:hover {
        background: var(--hu-hover-overlay);
        color: var(--hu-text);
      }

      .version-panel-close:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }

      .version-item {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-3xs);
        padding: var(--hu-space-sm) var(--hu-space-md);
        width: 100%;
        background: none;
        border: none;
        border-bottom: 1px solid var(--hu-border-subtle);
        text-align: left;
        cursor: pointer;
        font-family: var(--hu-font);
        color: var(--hu-text);
        transition: background var(--hu-duration-fast) var(--hu-ease-out);
      }

      .version-item:hover {
        background: var(--hu-hover-overlay);
      }

      .version-item:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: -2px;
      }

      .version-item.active {
        background: color-mix(in srgb, var(--hu-accent) 10%, transparent);
        border-left: 2px solid var(--hu-accent);
      }

      .version-item .v-label {
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-medium);
      }

      .version-item .v-meta {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-tertiary);
      }

      .detail-body {
        flex: 1;
        min-height: 0;
        display: flex;
        flex-direction: column;
      }

      .split-body {
        display: grid;
        grid-template-columns: 1fr 1fr;
        min-height: 0;
        flex: 1;
      }

      @media (max-width: 904px) /* --hu-breakpoint-medium */ {
        .split-body {
          grid-template-columns: 1fr;
        }
      }

      .split-body > * {
        min-height: 20rem;
        overflow: auto;
      }

      .split-divider {
        width: 1px;
        background: var(--hu-border-subtle);
      }

      hu-canvas-editor {
        border-right: 1px solid var(--hu-border-subtle);
      }

      hu-canvas {
        flex: 1;
        min-height: 0;
      }

      @media (prefers-reduced-motion: reduce) {
        * {
          transition-duration: 0s !important;
        }
      }
    `,
  ];

  override connectedCallback(): void {
    super.connectedCallback();
    this.gateway?.addEventListener(GatewayClient.EVENT_GATEWAY, this._onGateway);
  }

  override disconnectedCallback(): void {
    this.gateway?.removeEventListener(GatewayClient.EVENT_GATEWAY, this._onGateway);
    super.disconnectedCallback();
  }

  protected override onGatewaySwapped(
    previous: GatewayClient | null,
    current: GatewayClient,
  ): void {
    previous?.removeEventListener(GatewayClient.EVENT_GATEWAY, this._onGateway);
    current.addEventListener(GatewayClient.EVENT_GATEWAY, this._onGateway);
  }

  protected override async load(): Promise<void> {
    if (!this.gateway) {
      this._error = "Not connected to gateway";
      return;
    }
    this._loading = true;
    this._error = "";
    try {
      const res = await this.gateway.request<{
        canvases?: Array<{
          canvas_id: string;
          title: string;
          format: string;
          content: string;
          imports?: Record<string, string>;
          language?: string;
          version_seq?: number;
        }>;
      }>("canvas.list");

      if (res?.canvases && Array.isArray(res.canvases)) {
        this.canvases = res.canvases.map((c) => ({
          id: c.canvas_id,
          title: c.title || c.canvas_id,
          format: resolveFormat(c.format),
          content: c.content || "",
          imports: c.imports && typeof c.imports === "object" ? c.imports : {},
          language: c.language || "",
          versionSeq: c.version_seq ?? 0,
          status: "active" as const,
        }));
        if (this.canvases.length > 0 && !this.selectedId) {
          this.selectedId = this.canvases[0].id;
        }
      }
    } catch (err) {
      this._error = err instanceof Error ? err.message : "Failed to load canvases";
    }
    this._loading = false;
    this.lastLoadedAt = Date.now();
  }

  private _onEditorChanged(e: CustomEvent<{ content: string }>, canvasId: string): void {
    const content = e.detail.content;
    this.canvases = this.canvases.map((c) => (c.id === canvasId ? { ...c, content } : c));
    if (this._editTimer) clearTimeout(this._editTimer);
    this._editTimer = setTimeout(() => {
      void this.gateway?.request("canvas.edit", {
        canvas_id: canvasId,
        content,
      });
    }, 300);
  }

  private async _onUndo(): Promise<void> {
    const sel = this.selectedId;
    if (!sel) return;
    try {
      const res = await this.gateway?.request<{
        ok: boolean;
        canvas_id: string;
        content?: string;
        version_seq?: number;
      }>("canvas.undo", { canvas_id: sel });
      if (res?.ok && res.content !== undefined) {
        this.canvases = this.canvases.map((c) =>
          c.id === sel
            ? {
                ...c,
                content: res.content!,
                versionSeq: res.version_seq ?? c.versionSeq,
              }
            : c,
        );
      }
    } catch {
      /* ignore */
    }
  }

  private async _onRedo(): Promise<void> {
    const sel = this.selectedId;
    if (!sel) return;
    try {
      const res = await this.gateway?.request<{
        ok: boolean;
        canvas_id: string;
        content?: string;
        version_seq?: number;
      }>("canvas.redo", { canvas_id: sel });
      if (res?.ok && res.content !== undefined) {
        this.canvases = this.canvases.map((c) =>
          c.id === sel
            ? {
                ...c,
                content: res.content!,
                versionSeq: res.version_seq ?? c.versionSeq,
              }
            : c,
        );
      }
    } catch {
      /* ignore */
    }
  }

  private async _loadVersions(): Promise<void> {
    if (!this.selectedId || !this.gateway) return;
    try {
      const res = await this.gateway.request<{
        canvas_id: string;
        versions: Array<{ seq: number; created_at: number; summary: string }>;
      }>("canvas.versions", { canvas_id: this.selectedId });
      this._versions = res?.versions ?? [];
      this._versionsOpen = true;
    } catch {
      this._versions = [];
    }
  }

  private async _jumpToVersion(seq: number): Promise<void> {
    if (!this.selectedId || !this.gateway) return;
    try {
      const res = await this.gateway.request<{
        ok: boolean;
        content?: string;
        version_seq?: number;
      }>("canvas.get", { canvas_id: this.selectedId, version_seq: seq });
      if (res?.content !== undefined) {
        const sid = this.selectedId;
        this.canvases = this.canvases.map((c) =>
          c.id === sid ? { ...c, content: res.content!, versionSeq: res.version_seq ?? seq } : c,
        );
      }
    } catch {
      /* ignore */
    }
  }

  private _renderVersionPanel(currentSeq: number) {
    if (!this._versionsOpen) return nothing;
    return html`
      <div class="version-panel">
        <div class="version-panel-header">
          <span>Versions</span>
          <button
            class="version-panel-close"
            aria-label="Close versions"
            @click=${() => {
              this._versionsOpen = false;
            }}
          >
            ${icons.x}
          </button>
        </div>
        ${this._versions.map(
          (v) => html`
            <button
              class="version-item ${v.seq === currentSeq ? "active" : ""}"
              @click=${() => this._jumpToVersion(v.seq)}
            >
              <span class="v-label">v${v.seq}</span>
              <span class="v-meta">${v.summary}</span>
              <span class="v-meta">${new Date(v.created_at).toLocaleTimeString()}</span>
            </button>
          `,
        )}
      </div>
    `;
  }

  override render() {
    if (this._error) {
      return html`
        <h1>Live Canvas</h1>
        <hu-empty-state
          .icon=${icons.warning}
          heading="Connection Error"
          description=${this._error}
        >
          <hu-button variant="primary" @click=${() => this.load()}>Retry</hu-button>
        </hu-empty-state>
      `;
    }

    if (this._loading && this.canvases.length === 0) {
      return html`
        <h1>Live Canvas</h1>
        <div class="list" style="gap: var(--hu-space-md)">
          <hu-skeleton height="3.5rem"></hu-skeleton>
          <hu-skeleton height="3.5rem"></hu-skeleton>
        </div>
      `;
    }

    const selected = this.canvases.find((c) => c.id === this.selectedId) ?? null;
    const hasList = this.canvases.length > 0;

    return html`
      <div class="layout ${selected ? "has-detail" : ""}">
        <div class="list-col">
          <h1>Live Canvas</h1>
          ${!hasList
            ? html`
                <hu-empty-state
                  .icon=${icons.image}
                  heading="No active canvases"
                  description="When the agent uses the canvas tool, visual surfaces appear here."
                ></hu-empty-state>
              `
            : html`
                <div class="list" role="list">
                  ${this.canvases.map(
                    (c) => html`
                      <button
                        type="button"
                        class="row ${this.selectedId === c.id ? "selected" : ""}"
                        role="listitem"
                        @click=${() => {
                          this.selectedId = c.id;
                          this._viewMode = "preview";
                        }}
                      >
                        <div>
                          <div class="row-title">${c.title || c.id}</div>
                          <div class="row-meta">
                            ${c.format}${c.versionSeq > 0 ? ` · v${c.versionSeq}` : ""} ·
                            ${c.content ? "has content" : "empty"}
                          </div>
                        </div>
                      </button>
                    `,
                  )}
                </div>
              `}
        </div>
        ${selected ? this._renderDetail(selected) : nothing}
      </div>
    `;
  }

  private _renderPreview(c: CanvasEntry) {
    return html`
      <hu-canvas
        .title=${c.title || c.id}
        .content=${c.content}
        .format=${c.format}
        .imports=${c.imports}
        .language=${c.language}
      ></hu-canvas>
    `;
  }

  private _renderEditor(c: CanvasEntry) {
    return html`
      <hu-canvas-editor
        .content=${c.content}
        .format=${c.format}
        @canvas-content-changed=${(e: CustomEvent<{ content: string }>) =>
          this._onEditorChanged(e, c.id)}
      ></hu-canvas-editor>
    `;
  }

  private _formatLabels: Record<CanvasFormat, string> = {
    code: "Code",
    react: "React",
    svg: "SVG",
    mermaid: "Mermaid",
    html: "HTML",
    markdown: "Markdown",
    mockup: "Mockup",
  };

  private _renderDetail(c: CanvasEntry) {
    return html`
      <div class="detail">
        <div class="detail-header">
          <h2>${c.title || c.id}</h2>
          <div class="detail-controls">
            <div class="format-selector" role="tablist" aria-label="Canvas format">
              ${VALID_FORMATS.map(
                (f) => html`
                  <button
                    type="button"
                    role="tab"
                    class=${c.format === f ? "active" : ""}
                    aria-selected=${c.format === f ? "true" : "false"}
                    aria-label="${this._formatLabels[f]} format"
                    @click=${() => {
                      this.canvases = this.canvases.map((cv) =>
                        cv.id === c.id ? { ...cv, format: f } : cv,
                      );
                    }}
                  >
                    ${this._formatLabels[f]}
                  </button>
                `,
              )}
            </div>
            <div class="mode-toggle" role="tablist" aria-label="View mode">
              <button
                type="button"
                role="tab"
                aria-selected=${this._viewMode === "preview" ? "true" : "false"}
                aria-label="Preview mode"
                class=${this._viewMode === "preview" ? "active" : ""}
                @click=${() => {
                  this._viewMode = "preview";
                }}
              >
                Preview
              </button>
              <button
                type="button"
                role="tab"
                aria-selected=${this._viewMode === "code" ? "true" : "false"}
                aria-label="Code editor mode"
                class=${this._viewMode === "code" ? "active" : ""}
                @click=${() => {
                  this._viewMode = "code";
                }}
              >
                Code
              </button>
              <button
                type="button"
                role="tab"
                aria-selected=${this._viewMode === "split" ? "true" : "false"}
                aria-label="Split view mode"
                class=${this._viewMode === "split" ? "active" : ""}
                @click=${() => {
                  this._viewMode = "split";
                }}
              >
                Split
              </button>
            </div>
            ${c.versionSeq > 0
              ? html`
                  <button
                    class="version-info version-btn"
                    @click=${() => this._loadVersions()}
                    title="View version history"
                  >
                    v${c.versionSeq} ${icons.clock}
                  </button>
                  <hu-button variant="tonal" size="sm" @click=${() => this._onUndo()}>
                    Undo
                  </hu-button>
                  <hu-button variant="tonal" size="sm" @click=${() => this._onRedo()}>
                    Redo
                  </hu-button>
                `
              : nothing}
          </div>
        </div>
        <div class="detail-body" style="position: relative;">
          ${this._viewMode === "preview"
            ? this._renderPreview(c)
            : this._viewMode === "code"
              ? this._renderEditor(c)
              : html`
                  <div class="split-body">${this._renderEditor(c)} ${this._renderPreview(c)}</div>
                `}
          ${this._renderVersionPanel(c.versionSeq)}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-canvas-view": CanvasView;
  }
}
