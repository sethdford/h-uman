import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-file-preview.js";
import type { FilePreviewItem } from "./sc-file-preview.js";

const SUGGESTIONS = [
  { icon: "compass", title: "Explore the project", desc: "Architecture, patterns, and structure" },
  { icon: "code", title: "Write code", desc: "Python, scripts, web scrapers, and more" },
  { icon: "bug", title: "Debug an issue", desc: "Trace errors and find root causes" },
  { icon: "question", title: "Ask anything", desc: "General questions about capabilities" },
];

const LINE_HEIGHT = 24;
const MAX_LINES = 5;

@customElement("sc-composer")
export class ScComposer extends LitElement {
  static override styles = css`
    .input-wrap {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-md);
      background: color-mix(in srgb, var(--sc-bg-surface) 80%, transparent);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-xl);
      box-shadow: var(--sc-shadow-inset);
      transition:
        border-color var(--sc-duration-normal) var(--sc-ease-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out),
        outline var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }
    .input-wrap:focus-within {
      box-shadow:
        0 0 0 2px var(--sc-accent-subtle),
        var(--sc-shadow-inset);
    }
    .input-wrap.drag-over {
      outline: 2px dashed var(--sc-accent);
      outline-offset: -4px;
      background: color-mix(in srgb, var(--sc-accent) 4%, transparent);
    }
    .input-bar {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: flex-end;
    }
    .input-bar textarea {
      flex: 1;
      min-height: 44px;
      max-height: ${LINE_HEIGHT * MAX_LINES}px;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: transparent;
      border: none;
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      resize: none;
      line-height: ${LINE_HEIGHT}px;
    }
    .input-bar textarea:focus {
      outline: none;
    }
    .input-bar textarea:focus-visible {
      outline: none;
    }
    .input-bar textarea::placeholder {
      color: var(--sc-text-muted);
    }
    .attach-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 36px;
      height: 36px;
      min-width: 36px;
      min-height: 36px;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast),
        background var(--sc-duration-fast);
    }
    .attach-btn:hover:not(:disabled) {
      background: var(--sc-bg-elevated);
      color: var(--sc-accent);
    }
    .attach-btn:disabled {
      opacity: var(--sc-opacity-disabled, 0.5);
      cursor: not-allowed;
    }
    .attach-btn svg {
      width: 20px;
      height: 20px;
    }
    .send-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 40px;
      height: 40px;
      min-height: 40px;
      padding: 0;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius-full);
      cursor: pointer;
      font-family: var(--sc-font);
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-spring-micro, cubic-bezier(0.34, 1.56, 0.64, 1));
    }
    .send-btn svg {
      width: 18px;
      height: 18px;
    }
    .send-btn:hover:not(:disabled) {
      background: var(--sc-accent-hover);
    }
    .send-btn:active:not(:disabled) {
      transform: scale(0.92);
    }
    .send-btn:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }
    .send-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .stream-elapsed {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
    }
    .input-bar-actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .welcome-state {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--sc-space-lg);
      padding: var(--sc-space-lg) 0;
    }
    .welcome-heading {
      font-size: var(--sc-text-2xl, 1.5rem);
      font-weight: var(--sc-weight-light, 300);
      color: var(--sc-text-muted);
      letter-spacing: -0.01em;
    }
    .bento-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-sm);
      max-width: 420px;
      width: 100%;
    }
    .bento-card {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      cursor: pointer;
      text-align: left;
      font-family: var(--sc-font);
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
      animation: sc-card-enter var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
      animation-delay: calc(var(--bento-idx) * 80ms);
    }
    .bento-card:hover {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      box-shadow: var(--sc-shadow-glow-blue);
      transform: translateY(-1px);
    }
    .bento-card:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .bento-card:active {
      transform: scale(0.98);
    }
    .bento-icon {
      display: flex;
      color: var(--sc-accent);
    }
    .bento-icon svg {
      width: 24px;
      height: 24px;
    }
    .bento-title {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    .bento-desc {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      line-height: 1.4;
    }
    @keyframes sc-card-enter {
      from {
        opacity: 0;
        transform: translateY(8px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      .input-bar {
        flex-direction: column;
        align-items: stretch;
      }
      .bento-grid {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .input-wrap,
      .attach-btn,
      .send-btn,
      .bento-card {
        transition: none;
      }
      .bento-card {
        animation: none;
      }
    }
  `;

  @property({ type: String }) value = "";
  @property({ type: Boolean }) waiting = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean, attribute: "show-suggestions" }) showSuggestions = false;
  @property({ type: String, attribute: "stream-elapsed" }) streamElapsed = "";
  @property({ type: String }) placeholder = "Type a message...";

  @state() private _dragOver = false;
  @state() private _attachedFiles: FilePreviewItem[] = [];

  @query("#composer-textarea") private _textarea!: HTMLTextAreaElement;
  @query("#file-input") private _fileInput!: HTMLInputElement;

  private _resizeTextarea(): void {
    const el = this._textarea;
    if (!el) return;
    el.style.height = "auto";
    const maxHeight = LINE_HEIGHT * MAX_LINES;
    el.style.height = `${Math.min(el.scrollHeight, maxHeight)}px`;
  }

  private _handleInput(): void {
    const val = this._textarea?.value ?? "";
    this.value = val;
    this._resizeTextarea();
    this.dispatchEvent(
      new CustomEvent("sc-input-change", { bubbles: true, composed: true, detail: { value: val } }),
    );
  }

  private _handleKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this._emitSend();
    }
  }

  private _emitSend(): void {
    const msg = this.value.trim();
    if (!msg || this.waiting || this.disabled) return;
    const files = [...this._attachedFiles];
    this._attachedFiles = [];
    this.dispatchEvent(
      new CustomEvent("sc-send", {
        bubbles: true,
        composed: true,
        detail: { message: msg, files },
      }),
    );
  }

  private _handleAttachClick(): void {
    if (this.disabled) return;
    this._fileInput?.click();
  }

  private async _processFiles(files: File[]): Promise<void> {
    for (const file of files) {
      const item: FilePreviewItem = {
        name: file.name,
        size: file.size,
        type: file.type,
      };
      if (file.type.startsWith("image/")) {
        try {
          item.dataUrl = await new Promise<string>((resolve, reject) => {
            const r = new FileReader();
            r.onload = () => resolve(r.result as string);
            r.onerror = reject;
            r.readAsDataURL(file);
          });
        } catch {
          /* ignore */
        }
      }
      this._attachedFiles = [...this._attachedFiles, item];
    }
    this.requestUpdate();
  }

  private _handleFileChange(e: Event): void {
    const input = e.target as HTMLInputElement;
    const files = Array.from(input.files ?? []);
    input.value = "";
    if (files.length > 0) this._processFiles(files);
  }

  private _handleDragOver(e: DragEvent): void {
    e.preventDefault();
    this._dragOver = true;
  }

  private _handleDragLeave(): void {
    this._dragOver = false;
  }

  private _handleDrop(e: DragEvent): void {
    e.preventDefault();
    this._dragOver = false;
    const files = Array.from(e.dataTransfer?.files ?? []);
    if (files.length > 0) this._processFiles(files);
  }

  private _handleFileRemove(e: CustomEvent<{ index: number }>): void {
    const idx = e.detail.index;
    if (idx >= 0 && idx < this._attachedFiles.length) {
      this._attachedFiles = this._attachedFiles.filter((_, i) => i !== idx);
    }
  }

  private _handlePillClick(text: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-use-suggestion", {
        bubbles: true,
        composed: true,
        detail: { text },
      }),
    );
  }

  protected override updated(changed: Map<string, unknown>): void {
    if (changed.has("value") && this._textarea && this._textarea.value !== this.value) {
      this._textarea.value = this.value;
      this._resizeTextarea();
    }
  }

  override render() {
    const canSend = this.value.trim().length > 0 && !this.waiting && !this.disabled;

    return html`
      <div
        class="input-wrap ${this._dragOver ? "drag-over" : ""}"
        @dragover=${this._handleDragOver}
        @dragleave=${this._handleDragLeave}
        @drop=${this._handleDrop}
      >
        ${this._attachedFiles.length > 0
          ? html`
              <sc-file-preview
                .files=${this._attachedFiles}
                @sc-file-remove=${this._handleFileRemove}
              ></sc-file-preview>
            `
          : nothing}
        <div class="input-bar">
          <textarea
            id="composer-textarea"
            .value=${this.value}
            .placeholder=${this.placeholder}
            ?disabled=${this.disabled}
            @input=${this._handleInput}
            @keydown=${this._handleKeyDown}
          ></textarea>
          <input id="file-input" type="file" multiple hidden @change=${this._handleFileChange} />
          <div class="input-bar-actions">
            <button
              class="attach-btn"
              type="button"
              ?disabled=${this.disabled}
              @click=${this._handleAttachClick}
              aria-label="Attach file"
            >
              ${icons["file-text"]}
            </button>
            ${this.streamElapsed
              ? html`<span class="stream-elapsed">${this.streamElapsed}</span>`
              : nothing}
            <button
              class="send-btn"
              type="button"
              ?disabled=${!canSend}
              @click=${this._emitSend}
              aria-label="Send"
            >
              ${icons["arrow-up"]}
            </button>
          </div>
        </div>
        ${this.showSuggestions
          ? html`
              <div class="welcome-state">
                <span class="welcome-heading">How can I help?</span>
                <div class="bento-grid">
                  ${SUGGESTIONS.map(
                    (s, i) => html`
                      <button
                        class="bento-card"
                        type="button"
                        style="--bento-idx: ${i}"
                        aria-label=${s.title}
                        @click=${() => this._handlePillClick(s.title)}
                      >
                        <span class="bento-icon">${icons[s.icon as keyof typeof icons]}</span>
                        <span class="bento-title">${s.title}</span>
                        <span class="bento-desc">${s.desc}</span>
                      </button>
                    `,
                  )}
                </div>
              </div>
            `
          : nothing}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-composer": ScComposer;
  }
}
