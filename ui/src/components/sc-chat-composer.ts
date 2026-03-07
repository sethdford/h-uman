import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-file-preview.js";
import type { FilePreviewItem } from "./sc-file-preview.js";

const SUGGESTIONS = ["Explore the project", "Write code", "Debug an issue", "Ask anything"];
const SLASH_COMMANDS = [
  { command: "/tools", desc: "List available tools" },
  { command: "/system", desc: "View system prompt" },
  { command: "/export", desc: "Export conversation" },
  { command: "/clear", desc: "Clear conversation" },
];
const LINE_HEIGHT = 24;
const MAX_LINES = 5;

@customElement("sc-chat-composer")
export class ScChatComposer extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Boolean }) waiting = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean, attribute: "show-suggestions" }) showSuggestions = false;
  @property({ type: String, attribute: "stream-elapsed" }) streamElapsed = "";
  @property({ type: String }) placeholder = "Type a message...";
  @property({ type: String }) model = "";

  @state() private _dragOver = false;
  @state() private _attachedFiles: FilePreviewItem[] = [];
  @state() private _slashOpen = false;
  @state() private _slashIndex = 0;

  @query("#composer-textarea") private _textarea!: HTMLTextAreaElement;
  @query("#file-input") private _fileInput!: HTMLInputElement;

  static override styles = css`
    :host {
      display: block;
    }
    .composer {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-2xl, 24px);
      transition:
        border-color var(--sc-duration-fast),
        box-shadow var(--sc-duration-fast);
    }
    .composer:focus-within {
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    .composer.drag-over {
      outline: 2px dashed var(--sc-accent);
      outline-offset: -4px;
    }
    .suggestions {
      display: flex;
      gap: var(--sc-space-xs);
      overflow-x: auto;
      scrollbar-width: none;
      padding: var(--sc-space-xs) 0;
    }
    .suggestions::-webkit-scrollbar {
      display: none;
    }
    .pill {
      flex-shrink: 0;
      padding: var(--sc-space-xs) var(--sc-space-md);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font);
      color: var(--sc-text);
      cursor: pointer;
      white-space: nowrap;
      transition:
        border-color var(--sc-duration-fast),
        background var(--sc-duration-fast);
    }
    .pill:hover {
      border-color: var(--sc-accent);
      background: var(--sc-accent-subtle);
    }
    .pill:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .input-row {
      display: flex;
      align-items: flex-end;
      gap: var(--sc-space-sm);
    }
    .model-chip {
      flex-shrink: 0;
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast);
    }
    .model-chip:hover {
      color: var(--sc-text);
      border-color: var(--sc-border);
    }
    textarea {
      flex: 1;
      min-height: 44px;
      max-height: ${LINE_HEIGHT * MAX_LINES}px;
      padding: var(--sc-space-sm) 0;
      background: transparent;
      border: none;
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      resize: none;
      line-height: ${LINE_HEIGHT}px;
    }
    textarea:focus {
      outline: none;
    }
    textarea::placeholder {
      color: var(--sc-text-muted);
    }
    .actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
    }
    .icon-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 36px;
      height: 36px;
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
    .icon-btn:hover:not(:disabled) {
      background: var(--sc-bg-elevated);
      color: var(--sc-accent);
    }
    .icon-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .icon-btn svg {
      width: 20px;
      height: 20px;
    }
    .send-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 40px;
      height: 40px;
      min-width: 40px;
      padding: 0;
      border: none;
      border-radius: var(--sc-radius-full);
      cursor: pointer;
      font-family: var(--sc-font);
      transition:
        background var(--sc-duration-fast),
        transform var(--sc-duration-fast);
    }
    .send-btn.send {
      background: var(--sc-accent);
      color: var(--sc-bg);
    }
    .send-btn.stop {
      background: var(--sc-error);
      color: var(--sc-bg);
    }
    .send-btn svg {
      width: 18px;
      height: 18px;
    }
    .send-btn:hover:not(:disabled) {
      filter: brightness(1.1);
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
    .token-count {
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-faint);
      font-family: var(--sc-font);
      font-variant-numeric: tabular-nums;
    }
    .elapsed {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
    }
    .slash-popover {
      position: absolute;
      bottom: 100%;
      left: var(--sc-space-md);
      margin-bottom: var(--sc-space-xs);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-md);
      min-width: 200px;
      z-index: 10;
      overflow: hidden;
    }
    .slash-item {
      display: flex;
      flex-direction: column;
      gap: 2px;
      padding: var(--sc-space-sm) var(--sc-space-md);
      cursor: pointer;
      font-family: var(--sc-font);
      border: none;
      background: transparent;
      width: 100%;
      text-align: left;
      transition: background var(--sc-duration-fast);
    }
    .slash-item:hover,
    .slash-item.focused {
      background: var(--sc-bg-elevated);
    }
    .slash-cmd {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    .slash-desc {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    @media (max-width: 640px) {
      .input-row {
        flex-wrap: wrap;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .composer,
      .pill,
      .icon-btn,
      .send-btn {
        transition: none;
      }
    }
  `;

  private _resizeTextarea(): void {
    const el = this._textarea;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = `${Math.min(el.scrollHeight, LINE_HEIGHT * MAX_LINES)}px`;
  }

  private _handleInput(): void {
    const val = this._textarea?.value ?? "";
    this.value = val;
    this._resizeTextarea();
    this._slashOpen = val === "/";
    this._slashIndex = 0;
    this.dispatchEvent(
      new CustomEvent("sc-input-change", { bubbles: true, composed: true, detail: { value: val } }),
    );
  }

  private _handleKeyDown(e: KeyboardEvent): void {
    if (this._slashOpen) {
      if (e.key === "Escape") {
        this._slashOpen = false;
        return;
      }
      if (e.key === "ArrowDown") {
        e.preventDefault();
        this._slashIndex = Math.min(this._slashIndex + 1, SLASH_COMMANDS.length - 1);
        return;
      }
      if (e.key === "ArrowUp") {
        e.preventDefault();
        this._slashIndex = Math.max(this._slashIndex - 1, 0);
        return;
      }
      if (e.key === "Enter") {
        e.preventDefault();
        this._selectSlashCommand(SLASH_COMMANDS[this._slashIndex].command);
        return;
      }
    }
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      this._emitSend();
    }
  }

  private _selectSlashCommand(command: string): void {
    this._slashOpen = false;
    this.value = "";
    if (this._textarea) this._textarea.value = "";
    this.dispatchEvent(
      new CustomEvent("sc-slash-command", { bubbles: true, composed: true, detail: { command } }),
    );
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

  private _handleAbort(): void {
    this.dispatchEvent(new CustomEvent("sc-abort", { bubbles: true, composed: true }));
  }

  private _handlePaste(e: ClipboardEvent): void {
    const items = e.clipboardData?.items;
    if (!items) return;
    for (const item of Array.from(items)) {
      if (item.type.startsWith("image/")) {
        e.preventDefault();
        const file = item.getAsFile();
        if (file) this._processFiles([file]);
        return;
      }
    }
  }

  private _handleAttachClick(): void {
    if (this.disabled) return;
    this._fileInput?.click();
  }

  private async _processFiles(files: File[]): Promise<void> {
    for (const file of files) {
      const item: FilePreviewItem = { name: file.name, size: file.size, type: file.type };
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
    if (idx >= 0 && idx < this._attachedFiles.length)
      this._attachedFiles = this._attachedFiles.filter((_, i) => i !== idx);
  }

  private _handlePillClick(text: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-use-suggestion", { bubbles: true, composed: true, detail: { text } }),
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
    const tokenEstimate = this.value.length > 500 ? Math.ceil(this.value.length / 4) : 0;
    return html`
      <div style="position:relative;">
        ${this._slashOpen
          ? html`
              <div class="slash-popover">
                ${SLASH_COMMANDS.map(
                  (cmd, i) => html`
                    <button
                      class="slash-item ${i === this._slashIndex ? "focused" : ""}"
                      @click=${() => this._selectSlashCommand(cmd.command)}
                    >
                      <span class="slash-cmd">${cmd.command}</span
                      ><span class="slash-desc">${cmd.desc}</span>
                    </button>
                  `,
                )}
              </div>
            `
          : nothing}
        ${this.showSuggestions
          ? html`
              <div class="suggestions">
                ${SUGGESTIONS.map(
                  (s) =>
                    html`<button
                      class="pill"
                      type="button"
                      @click=${() => this._handlePillClick(s)}
                    >
                      ${s}
                    </button>`,
                )}
              </div>
            `
          : nothing}
        <div
          class="composer ${this._dragOver ? "drag-over" : ""}"
          @dragover=${this._handleDragOver}
          @dragleave=${this._handleDragLeave}
          @drop=${this._handleDrop}
        >
          ${this._attachedFiles.length > 0
            ? html`<sc-file-preview
                .files=${this._attachedFiles}
                @sc-file-remove=${this._handleFileRemove}
              ></sc-file-preview>`
            : nothing}
          <div class="input-row">
            ${this.model
              ? html`<button
                  class="model-chip"
                  type="button"
                  @click=${() =>
                    this.dispatchEvent(
                      new CustomEvent("sc-model-select", { bubbles: true, composed: true }),
                    )}
                >
                  ${this.model}
                </button>`
              : nothing}
            <textarea
              id="composer-textarea"
              .value=${this.value}
              .placeholder=${this.placeholder}
              ?disabled=${this.disabled}
              @input=${this._handleInput}
              @keydown=${this._handleKeyDown}
              @paste=${this._handlePaste}
            ></textarea>
            <input id="file-input" type="file" multiple hidden @change=${this._handleFileChange} />
            <div class="actions">
              <button
                class="icon-btn"
                type="button"
                ?disabled=${this.disabled}
                @click=${this._handleAttachClick}
                aria-label="Attach file"
              >
                ${icons["file-text"]}
              </button>
              ${tokenEstimate > 0
                ? html`<span class="token-count">~${tokenEstimate} tokens</span>`
                : nothing}
              ${this.streamElapsed
                ? html`<span class="elapsed">${this.streamElapsed}</span>`
                : nothing}
              ${this.waiting
                ? html`<button
                    class="send-btn stop"
                    type="button"
                    @click=${this._handleAbort}
                    aria-label="Stop generating"
                  >
                    ${icons.stop ?? icons.x}
                  </button>`
                : html`<button
                    class="send-btn send"
                    type="button"
                    ?disabled=${!canSend}
                    @click=${this._emitSend}
                    aria-label="Send"
                  >
                    ${icons["arrow-up"]}
                  </button>`}
            </div>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-chat-composer": ScChatComposer;
  }
}
