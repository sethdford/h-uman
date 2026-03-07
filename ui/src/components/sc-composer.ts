import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-file-preview.js";
import type { FilePreviewItem } from "./sc-file-preview.js";

const SUGGESTIONS = [
  "Explain how this project is architected",
  "Write a Python web scraper",
  "Help me debug an issue",
  "What can you do?",
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
      background: var(--sc-bg-surface);
      border-top: 1px solid var(--sc-border);
      transition:
        outline var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
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
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      resize: none;
      line-height: ${LINE_HEIGHT}px;
    }
    .input-bar textarea:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    .input-bar textarea:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .input-bar textarea::placeholder {
      color: var(--sc-text-muted);
    }
    .attach-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      min-width: 44px;
      min-height: 44px;
      padding: 0;
      background: transparent;
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast),
        background var(--sc-duration-fast);
    }
    .attach-btn:hover:not(:disabled) {
      color: var(--sc-accent);
      border-color: var(--sc-accent);
      background: var(--sc-accent-subtle);
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
      padding: var(--sc-space-sm) var(--sc-space-lg);
      min-height: 44px;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      font-size: var(--sc-text-base);
      font-family: var(--sc-font);
      transition: background var(--sc-duration-fast) var(--sc-ease-out);
    }
    .send-btn:hover:not(:disabled) {
      background: var(--sc-accent-hover);
    }
    .send-btn:disabled {
      opacity: var(--sc-opacity-disabled, 0.5);
      cursor: not-allowed;
    }
    .send-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .char-count {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .suggested-prompts {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
      justify-content: center;
      margin-top: var(--sc-space-xs);
    }
    .prompt-pill {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-full);
      padding: var(--sc-space-xs) var(--sc-space-md);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      cursor: pointer;
      transition: all var(--sc-duration-fast) var(--sc-ease-out);
      white-space: nowrap;
    }
    .prompt-pill:hover {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      color: var(--sc-accent);
    }
    .prompt-pill:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .stream-elapsed {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
    }
    @media (max-width: 640px) {
      .input-bar {
        flex-direction: column;
        align-items: stretch;
      }
      .send-btn {
        min-height: 40px;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .input-wrap,
      .attach-btn,
      .send-btn,
      .prompt-pill {
        transition: none;
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
          <button
            class="attach-btn"
            type="button"
            ?disabled=${this.disabled}
            @click=${this._handleAttachClick}
            aria-label="Attach file"
          >
            ${icons["file-text"]}
          </button>
          <button
            class="send-btn"
            type="button"
            ?disabled=${!canSend}
            @click=${this._emitSend}
            aria-label="Send"
          >
            Send
          </button>
        </div>
        <div class="char-count">${this.value.length} characters</div>
        ${this.streamElapsed
          ? html`<span class="stream-elapsed">${this.streamElapsed}</span>`
          : nothing}
        ${this.showSuggestions
          ? html`
              <div class="suggested-prompts">
                ${SUGGESTIONS.map(
                  (text) =>
                    html`<button
                      class="prompt-pill"
                      type="button"
                      @click=${() => this._handlePillClick(text)}
                    >
                      ${text}
                    </button>`,
                )}
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
