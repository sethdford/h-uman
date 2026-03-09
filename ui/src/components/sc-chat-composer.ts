import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state, query } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-file-preview.js";
import type { FilePreviewItem } from "./sc-file-preview.js";

const SUGGESTIONS = ["Explore the project", "Write code", "Debug an issue", "Ask anything"];

const SLASH_COMMANDS: Array<{ command: string; desc: string; icon: string }> = [
  { command: "/model", desc: "Switch AI model", icon: "cpu" },
  { command: "/persona", desc: "Change persona profile", icon: "user" },
  { command: "/memory", desc: "Search memory", icon: "brain" },
  { command: "/attach", desc: "Attach a file", icon: "paperclip" },
  { command: "/export", desc: "Export conversation", icon: "export" },
  { command: "/clear", desc: "Clear conversation", icon: "trash" },
  { command: "/help", desc: "Show available commands", icon: "question" },
];

const DEMO_FILES = [
  "main.c",
  "config.json",
  "README.md",
  "persona.json",
  "tools.json",
  "security_policy.json",
  "channels/telegram.c",
  "channels/discord.c",
  "channels/slack.c",
  "providers/openai.c",
];

/** Simple fuzzy match: query chars appear in order in str (case-insensitive). */
function fuzzyMatch(str: string, query: string): boolean {
  if (!query) return true;
  const s = str.toLowerCase();
  const q = query.toLowerCase();
  let j = 0;
  for (let i = 0; i < s.length && j < q.length; i++) {
    if (s[i] === q[j]) j++;
  }
  return j === q.length;
}
/** Matches --sc-space-2xl (24px). JS needs pixel values; CSS vars not usable in getComputedStyle-free calculations. */
const LINE_HEIGHT = 24;
/** Max visible lines before textarea scrolls. */
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
  @property({ type: Boolean, attribute: "voice-active" }) voiceActive = false;
  @property({ type: Boolean, attribute: "voice-supported" }) voiceSupported = true;

  @state() private _dragOver = false;
  @state() private _attachedFiles: FilePreviewItem[] = [];
  @state() private _slashOpen = false;
  @state() private _slashIndex = 0;
  @state() private _slashQuery = "";
  @state() private _mentionQuery = "";
  @state() private _mentionResults: string[] = [];
  @state() private _mentionActive = false;
  @state() private _mentionIndex = 0;
  @state() private _mentionedFiles: string[] = [];
  @state() private _contextChips: Array<{
    type: "file" | "image" | "code";
    name: string;
    id: string;
  }> = [];
  @state() private _contextChipsExpanded = false;

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
      background: color-mix(in srgb, var(--sc-bg-surface) 65%, transparent);
      backdrop-filter: blur(var(--sc-space-md));
      -webkit-backdrop-filter: blur(var(--sc-space-md));
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-xl);
      transition:
        border-color var(--sc-duration-fast),
        box-shadow var(--sc-duration-fast);
    }
    .composer:focus-within {
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 2px var(--sc-accent-subtle);
    }
    .composer.drag-over {
      outline: 2px dashed var(--sc-accent);
      outline-offset: calc(-1 * var(--sc-space-xs));
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
      min-height: 2.75rem;
      max-height: ${(LINE_HEIGHT * MAX_LINES) / 16}rem;
      padding: var(--sc-space-sm) 0;
      background: transparent;
      border: none;
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-base);
      resize: none;
      line-height: ${LINE_HEIGHT / 16}rem;
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
      width: var(--sc-icon-2xl);
      height: var(--sc-icon-2xl);
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
      background: var(--sc-hover-overlay);
      color: var(--sc-accent);
    }
    .icon-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .icon-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .icon-btn svg {
      width: 1.25rem;
      height: 1.25rem;
    }
    .mic-btn.active {
      background: var(--sc-accent-subtle);
      color: var(--sc-accent);
    }
    @keyframes sc-send-spring {
      0% {
        transform: scale(1);
      }
      20% {
        transform: scale(0.88);
      }
      50% {
        transform: scale(1.08);
      }
      70% {
        transform: scale(0.97);
      }
      100% {
        transform: scale(1);
      }
    }
    @keyframes sc-send-glow {
      0%,
      100% {
        box-shadow: 0 0 0 0 color-mix(in srgb, var(--sc-accent) 0%, transparent);
      }
      50% {
        box-shadow: 0 0 var(--sc-space-md) var(--sc-space-2xs)
          color-mix(in srgb, var(--sc-accent) 25%, transparent);
      }
    }
    .send-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 2.5rem;
      height: 2.5rem;
      min-width: 2.5rem;
      padding: 0;
      border: none;
      border-radius: var(--sc-radius-full);
      cursor: pointer;
      font-family: var(--sc-font);
      transition:
        background var(--sc-duration-fast),
        transform var(--sc-duration-normal) var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)),
        box-shadow var(--sc-duration-fast);
    }
    .send-btn.send {
      background: var(--sc-accent);
      color: var(--sc-on-accent);
    }
    .send-btn.stop {
      background: var(--sc-error);
      color: var(--sc-bg);
    }
    .send-btn svg {
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
    }
    .send-btn:not(:disabled) {
      animation: sc-send-glow var(--sc-duration-slowest) var(--sc-ease-in-out) infinite;
    }
    .send-btn:hover:not(:disabled) {
      filter: brightness(1.1);
      box-shadow: 0 0 var(--sc-space-md) var(--sc-space-2xs)
        color-mix(in srgb, var(--sc-accent) 30%, transparent);
      transform: scale(1.06);
    }
    .send-btn:active:not(:disabled) {
      animation:
        sc-send-spring var(--sc-duration-normal)
          var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)),
        sc-send-glow var(--sc-duration-slowest) var(--sc-ease-in-out) infinite;
    }
    .send-btn:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }
    .send-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .elapsed {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-variant-numeric: tabular-nums;
    }
    .composer-wrap {
      position: relative;
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
      min-width: 12.5rem;
      z-index: 10;
      overflow: hidden;
    }
    .slash-item {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
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
      background: var(--sc-hover-overlay);
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
    /* 3a: @-mention dropdown */
    .mention-dropdown {
      position: absolute;
      bottom: 100%;
      left: var(--sc-space-md);
      margin-bottom: var(--sc-space-xs);
      background: var(--sc-surface-container-high);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-md);
      min-width: 12.5rem;
      max-height: 12.5rem;
      overflow-y: auto;
      z-index: 10;
    }
    .mention-item {
      display: block;
      width: 100%;
      padding: var(--sc-space-sm) var(--sc-space-md);
      text-align: left;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      background: transparent;
      border: none;
      cursor: pointer;
      transition: background var(--sc-duration-fast);
    }
    .mention-item:hover,
    .mention-item.active {
      background: var(--sc-hover-overlay);
    }
    .mention-item .match {
      color: var(--sc-accent);
      font-weight: var(--sc-weight-medium);
    }
    /* 3b: Command palette */
    .command-palette {
      position: absolute;
      bottom: 100%;
      left: var(--sc-space-md);
      margin-bottom: var(--sc-space-xs);
      background: var(--sc-surface-container-high);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-md);
      min-width: 18rem;
      max-height: 16rem;
      overflow-y: auto;
      z-index: 10;
    }
    .command-item {
      display: flex;
      align-items: flex-start;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      width: 100%;
      text-align: left;
      font-family: var(--sc-font);
      background: transparent;
      border: none;
      cursor: pointer;
      transition: background var(--sc-duration-fast);
    }
    .command-item:hover,
    .command-item.active {
      background: var(--sc-hover-overlay);
    }
    .command-item .cmd-icon {
      flex-shrink: 0;
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
      color: var(--sc-text-muted);
    }
    .command-item .cmd-icon svg {
      width: 100%;
      height: 100%;
    }
    .command-item .cmd-name {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    .command-item .cmd-desc {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .command-item .cmd-content {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }
    /* 3c: Context chips bar */
    .context-bar {
      display: flex;
      flex-direction: row;
      flex-wrap: wrap;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-xs) 0;
      align-items: center;
    }
    .context-chip {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      background: var(--sc-surface-container);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text);
    }
    .context-chip.type-file {
      border-color: color-mix(in srgb, var(--sc-accent) 30%, transparent);
    }
    .context-chip.type-image {
      border-color: color-mix(in srgb, var(--sc-accent-secondary) 30%, transparent);
    }
    .context-chip.type-code {
      border-color: color-mix(in srgb, var(--sc-accent-tertiary) 30%, transparent);
    }
    .context-chip .chip-icon {
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .context-chip .chip-icon svg {
      width: 100%;
      height: 100%;
    }
    .context-chip .chip-close {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
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
    .context-chip .chip-close:hover {
      color: var(--sc-text);
      background: var(--sc-hover-overlay);
    }
    .context-chip .chip-close svg {
      width: 0.75rem;
      height: 0.75rem;
    }
    .context-more {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      cursor: pointer;
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      font-family: var(--sc-font);
      background: transparent;
      border: none;
      border-radius: var(--sc-radius);
    }
    .context-more:hover {
      color: var(--sc-accent);
    }
    @media (prefers-reduced-transparency: reduce) {
      .composer {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--sc-bg-surface);
      }
    }
    @media (max-width: 640px) /* --sc-breakpoint-md */ {
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
      .send-btn:not(:disabled) {
        animation: none;
      }
    }
  `;

  focus(): void {
    this._textarea?.focus();
  }

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

    // Slash command: show when input starts with /
    this._slashOpen = val.startsWith("/");
    this._slashQuery = val.slice(1).split(/\s/)[0] ?? "";
    if (this._slashOpen) this._slashIndex = 0;

    // @-mention: find last @ and capture query after it
    const lastAt = val.lastIndexOf("@");
    if (lastAt >= 0) {
      const afterAt = val.slice(lastAt + 1);
      const spaceOrEnd = afterAt.search(/\s|$/);
      const query = afterAt.slice(0, spaceOrEnd === -1 ? afterAt.length : spaceOrEnd);
      this._mentionActive = true;
      this._mentionQuery = query;
      this._mentionResults = DEMO_FILES.filter((f) => fuzzyMatch(f, query));
      this._mentionIndex = 0;
    } else {
      this._mentionActive = false;
      this._mentionQuery = "";
      this._mentionResults = [];
    }

    this.dispatchEvent(
      new CustomEvent("sc-input-change", { bubbles: true, composed: true, detail: { value: val } }),
    );
  }

  private _getFilteredSlashCommands(): typeof SLASH_COMMANDS {
    if (!this._slashQuery) return SLASH_COMMANDS;
    const q = this._slashQuery.toLowerCase();
    return SLASH_COMMANDS.filter((c) => fuzzyMatch(c.command.slice(1), q));
  }

  private _handleKeyDown(e: KeyboardEvent): void {
    const filteredCommands = this._getFilteredSlashCommands();

    if (this._mentionActive && this._mentionResults.length > 0) {
      if (e.key === "Escape") {
        this._mentionActive = false;
        return;
      }
      if (e.key === "ArrowDown") {
        e.preventDefault();
        this._mentionIndex = Math.min(this._mentionIndex + 1, this._mentionResults.length - 1);
        return;
      }
      if (e.key === "ArrowUp") {
        e.preventDefault();
        this._mentionIndex = Math.max(this._mentionIndex - 1, 0);
        return;
      }
      if (e.key === "Enter") {
        e.preventDefault();
        this._selectMention(this._mentionResults[this._mentionIndex]);
        return;
      }
    }

    if (this._slashOpen && filteredCommands.length > 0) {
      if (e.key === "Escape") {
        this._slashOpen = false;
        return;
      }
      if (e.key === "ArrowDown") {
        e.preventDefault();
        this._slashIndex = Math.min(this._slashIndex + 1, filteredCommands.length - 1);
        return;
      }
      if (e.key === "ArrowUp") {
        e.preventDefault();
        this._slashIndex = Math.max(this._slashIndex - 1, 0);
        return;
      }
      if (e.key === "Enter") {
        e.preventDefault();
        this._selectSlashCommand(filteredCommands[this._slashIndex].command);
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
    if (command === "/attach") {
      this._fileInput?.click();
    } else {
      this.dispatchEvent(
        new CustomEvent("sc-slash-command", { bubbles: true, composed: true, detail: { command } }),
      );
    }
  }

  private _selectMention(filename: string): void {
    const val = this._textarea?.value ?? "";
    const lastAt = val.lastIndexOf("@");
    if (lastAt < 0) return;
    const before = val.slice(0, lastAt);
    const afterAt = val.slice(lastAt + 1);
    const space = afterAt.search(/\s|$/);
    const after = space === -1 ? "" : afterAt.slice(space);
    const newVal = `${before}@${filename}${after}`;
    this.value = newVal;
    if (this._textarea) this._textarea.value = newVal;
    this._resizeTextarea();
    this._mentionActive = false;
    this._mentionQuery = "";
    this._mentionResults = [];
    if (!this._mentionedFiles.includes(filename)) {
      this._mentionedFiles = [...this._mentionedFiles, filename];
      this._syncContextChips();
    }
  }

  private _syncContextChips(): void {
    const chips: Array<{ type: "file" | "image" | "code"; name: string; id: string }> = [];
    for (const f of this._mentionedFiles) {
      chips.push({ type: "file", name: f, id: `mention-${f}` });
    }
    for (let i = 0; i < this._attachedFiles.length; i++) {
      const f = this._attachedFiles[i];
      const type = f.type?.startsWith("image/")
        ? "image"
        : f.name.match(/\.(c|h|ts|js|json)$/)
          ? "code"
          : "file";
      chips.push({ type, name: f.name, id: `attach-${i}-${f.name}` });
    }
    this._contextChips = chips;
  }

  private _removeContextChip(id: string): void {
    if (id.startsWith("mention-")) {
      const name = id.slice(8);
      this._mentionedFiles = this._mentionedFiles.filter((f) => f !== name);
    } else if (id.startsWith("attach-")) {
      const match = id.match(/^attach-(\d+)-/);
      if (match) {
        const idx = parseInt(match[1], 10);
        this._attachedFiles = this._attachedFiles.filter((_, i) => i !== idx);
      }
    }
    this._syncContextChips();
  }

  private _emitSend(): void {
    const msg = this.value.trim();
    if (!msg || this.waiting || this.disabled) return;
    const files = [...this._attachedFiles];
    const mentionedFiles = [...this._mentionedFiles];
    this._attachedFiles = [];
    this._mentionedFiles = [];
    this._syncContextChips();
    this.dispatchEvent(
      new CustomEvent("sc-send", {
        bubbles: true,
        composed: true,
        detail: { message: msg, files, mentionedFiles },
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

  private _handleMicClick(): void {
    if (this.disabled) return;
    if (this.voiceActive) {
      this.dispatchEvent(new CustomEvent("sc-voice-stop", { bubbles: true, composed: true }));
    } else {
      this.dispatchEvent(new CustomEvent("sc-voice-start", { bubbles: true, composed: true }));
    }
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
    this._syncContextChips();
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
      this._syncContextChips();
    }
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

  private _renderMentionMatch(filename: string): ReturnType<typeof html> {
    if (!this._mentionQuery) return html`${filename}`;
    const lower = filename.toLowerCase();
    const q = this._mentionQuery.toLowerCase();
    const parts: (string | ReturnType<typeof html>)[] = [];
    let j = 0;
    for (let i = 0; i < lower.length; i++) {
      if (j < q.length && lower[i] === q[j]) {
        parts.push(html`<span class="match">${filename[i]}</span>`);
        j++;
      } else {
        const run: string[] = [];
        while (i < lower.length && (j >= q.length || lower[i] !== q[j])) {
          run.push(filename[i]);
          i++;
        }
        i--;
        if (run.length) parts.push(run.join(""));
      }
    }
    return html`${parts}`;
  }

  override render() {
    const canSend = this.value.trim().length > 0 && !this.waiting && !this.disabled;
    const filteredCommands = this._getFilteredSlashCommands();
    const visibleChips =
      this._contextChipsExpanded || this._contextChips.length <= 3
        ? this._contextChips
        : this._contextChips.slice(0, 3);
    const moreCount = this._contextChips.length - 3;

    return html`
      <div class="composer-wrap">
        ${this._mentionActive && this._mentionResults.length > 0
          ? html`
              <div class="mention-dropdown" role="listbox" aria-label="File mentions">
                ${this._mentionResults.map(
                  (f, i) => html`
                    <button
                      class="mention-item ${i === this._mentionIndex ? "active" : ""}"
                      role="option"
                      ?aria-selected=${i === this._mentionIndex}
                      @click=${() => this._selectMention(f)}
                    >
                      ${this._renderMentionMatch(f)}
                    </button>
                  `,
                )}
              </div>
            `
          : nothing}
        ${this._slashOpen && filteredCommands.length > 0
          ? html`
              <div class="command-palette" role="listbox" aria-label="Commands">
                ${filteredCommands.map(
                  (cmd, i) => html`
                    <button
                      class="command-item ${i === this._slashIndex ? "active" : ""}"
                      role="option"
                      ?aria-selected=${i === this._slashIndex}
                      @click=${() => this._selectSlashCommand(cmd.command)}
                    >
                      <span class="cmd-icon"
                        >${(icons as Record<string, unknown>)[cmd.icon] ?? icons["file-text"]}</span
                      >
                      <div class="cmd-content">
                        <span class="cmd-name">${cmd.command}</span>
                        <span class="cmd-desc">${cmd.desc}</span>
                      </div>
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
          ${this._contextChips.length > 0
            ? html`
                <div class="context-bar">
                  ${visibleChips.map(
                    (chip) => html`
                      <span class="context-chip type-${chip.type}">
                        <span class="chip-icon"
                          >${chip.type === "image"
                            ? icons.image
                            : chip.type === "code"
                              ? icons.code
                              : icons["file-text"]}</span
                        >
                        <span>${chip.name}</span>
                        <button
                          class="chip-close"
                          type="button"
                          aria-label="Remove ${chip.name}"
                          @click=${() => this._removeContextChip(chip.id)}
                        >
                          ${icons.x}
                        </button>
                      </span>
                    `,
                  )}
                  ${moreCount > 0 && !this._contextChipsExpanded
                    ? html`
                        <button
                          class="context-more"
                          type="button"
                          @click=${() => (this._contextChipsExpanded = true)}
                        >
                          and ${moreCount} more
                        </button>
                      `
                    : this._contextChipsExpanded && moreCount > 0
                      ? html`
                          <button
                            class="context-more"
                            type="button"
                            @click=${() => (this._contextChipsExpanded = false)}
                          >
                            show less
                          </button>
                        `
                      : nothing}
                </div>
              `
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
              ${this.voiceSupported
                ? html`<button
                    class="icon-btn mic-btn ${this.voiceActive ? "active" : ""}"
                    type="button"
                    ?disabled=${this.disabled}
                    @click=${this._handleMicClick}
                    aria-label="Voice input"
                  >
                    ${icons.mic}
                  </button>`
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
