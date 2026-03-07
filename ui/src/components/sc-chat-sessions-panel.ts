import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { formatRelative } from "../utils.js";

export interface ChatSession {
  id: string;
  title: string;
  ts: number;
  active: boolean;
}

@customElement("sc-chat-sessions-panel")
export class ScChatSessionsPanel extends LitElement {
  @property({ type: Array }) sessions: ChatSession[] = [];

  @property({ type: Boolean, reflect: true }) open = false;

  @state() private _searchQuery = "";

  @state() private _focusedIndex = -1;

  private get _filteredSessions(): ChatSession[] {
    const q = this._searchQuery.toLowerCase();
    if (!q) return this.sessions;
    return this.sessions.filter((s) => s.title.toLowerCase().includes(q));
  }

  static override styles = css`
    :host {
      --_panel-width: 16.25rem;
      --_panel-width-expanded: 17.5rem;
      display: block;
      width: 0;
      overflow: hidden;
      flex-shrink: 0;
      transition: width var(--sc-duration-normal) var(--sc-ease-spring);
    }

    :host([open]) {
      width: var(--_panel-width);
    }

    .panel {
      width: var(--_panel-width);
      height: 100%;
      display: flex;
      flex-direction: column;
      background: var(--sc-bg-surface);
      border-right: 1px solid var(--sc-border-subtle);
    }

    @media (max-width: 768px) /* --sc-breakpoint-lg */ {
      :host([open]) {
        position: fixed;
        left: 0;
        top: 0;
        bottom: 0;
        z-index: 20;
        width: var(--_panel-width-expanded);
      }
    }

    .new-chat-btn {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      width: 100%;
      padding: var(--sc-space-md);
      margin: var(--sc-space-sm);
      background: transparent;
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .new-chat-btn:hover {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      color: var(--sc-accent-text, var(--sc-accent));
    }

    .new-chat-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .new-chat-btn svg {
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
      flex-shrink: 0;
    }

    .search-wrap {
      padding: 0 var(--sc-space-sm);
      margin-bottom: var(--sc-space-xs);
    }

    .search-input {
      box-sizing: border-box;
      width: 100%;
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: var(--sc-bg-inset);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      outline: none;
      transition: border-color var(--sc-duration-fast);
    }

    .search-input:focus {
      border-color: var(--sc-accent);
    }

    .search-input:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .search-input::placeholder {
      color: var(--sc-text-faint);
    }

    .session-list {
      flex: 1;
      overflow-y: auto;
      padding: 0 var(--sc-space-sm) var(--sc-space-md);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .group-label {
      position: sticky;
      top: 0;
      display: block;
      font-size: var(--sc-text-2xs, 0.625rem);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      padding: var(--sc-space-sm) var(--sc-space-md);
      margin-top: var(--sc-space-xs);
      background: var(--sc-bg-surface);
      z-index: 1;
    }

    .session-group:first-child .group-label {
      margin-top: 0;
    }

    .session-item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-radius: var(--sc-radius);
      border-left: 3px solid transparent;
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out);
      text-align: left;
      background: transparent;
      border-right: none;
      border-top: none;
      border-bottom: none;
      width: 100%;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
    }

    .session-item:hover {
      background: var(--sc-bg-elevated);
    }

    .session-item.active {
      border-left-color: var(--sc-accent-subtle);
      background: color-mix(in srgb, var(--sc-accent) 8%, var(--sc-bg-surface));
    }

    .session-item.focused {
      background: var(--sc-bg-elevated);
    }

    .session-item:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .session-content {
      flex: 1;
      min-width: 0;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .session-title {
      font-weight: var(--sc-weight-medium);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .session-ts {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .delete-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-lg);
      height: var(--sc-icon-lg);
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      opacity: 0;
      flex-shrink: 0;
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .session-item:hover .delete-btn {
      opacity: 1;
    }

    .delete-btn:hover {
      color: var(--sc-error);
      background: var(--sc-error-dim);
    }

    .delete-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
      opacity: 1;
    }

    .delete-btn svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .empty-state {
      padding: var(--sc-space-lg);
      text-align: center;
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }

    .empty-sessions {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-xl) var(--sc-space-md);
      text-align: center;
    }
    .empty-sessions .empty-text {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }
    .empty-sessions .empty-hint {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      font-family: var(--sc-font);
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        transition: none;
      }
      .new-chat-btn,
      .session-item,
      .delete-btn,
      .search-input {
        transition: none;
      }
    }
  `;

  private _onNewChat(): void {
    this.dispatchEvent(
      new CustomEvent("sc-session-new", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onSelect(id: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-session-select", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
  }

  private _onDelete(e: Event, id: string): void {
    e.stopPropagation();
    this.dispatchEvent(
      new CustomEvent("sc-session-delete", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
  }

  private _groupSessions(
    sessions: ChatSession[],
  ): Array<{ label: string; sessions: ChatSession[] }> {
    const now = Date.now();
    const day = 86400000;
    const today: ChatSession[] = [];
    const yesterday: ChatSession[] = [];
    const thisWeek: ChatSession[] = [];
    const thisMonth: ChatSession[] = [];
    const older: ChatSession[] = [];

    for (const s of sessions) {
      const age = now - s.ts;
      if (age < day) today.push(s);
      else if (age < 2 * day) yesterday.push(s);
      else if (age < 7 * day) thisWeek.push(s);
      else if (age < 30 * day) thisMonth.push(s);
      else older.push(s);
    }

    const groups: Array<{ label: string; sessions: ChatSession[] }> = [];
    if (today.length) groups.push({ label: "Today", sessions: today });
    if (yesterday.length) groups.push({ label: "Yesterday", sessions: yesterday });
    if (thisWeek.length) groups.push({ label: "This Week", sessions: thisWeek });
    if (thisMonth.length) groups.push({ label: "This Month", sessions: thisMonth });
    if (older.length) groups.push({ label: "Older", sessions: older });
    return groups;
  }

  private _onListKeydown(e: KeyboardEvent): void {
    const groups = this._groupSessions(this._filteredSessions);
    const flatSessions = groups.flatMap((g) => g.sessions);
    if (e.key === "ArrowDown") {
      e.preventDefault();
      this._focusedIndex = Math.min(this._focusedIndex + 1, flatSessions.length - 1);
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      this._focusedIndex = Math.max(this._focusedIndex - 1, 0);
    } else if (e.key === "Enter" && this._focusedIndex >= 0 && flatSessions[this._focusedIndex]) {
      e.preventDefault();
      this._onSelect(flatSessions[this._focusedIndex].id);
    } else if (e.key === "Escape") {
      this._focusedIndex = -1;
    }
  }

  private _startRename(e: Event, _s: ChatSession): void {
    const el = e.target as HTMLElement;
    el.contentEditable = "true";
    el.focus();
    const range = document.createRange();
    range.selectNodeContents(el);
    window.getSelection()?.removeAllRanges();
    window.getSelection()?.addRange(range);
  }

  private _finishRename(e: Event, id: string): void {
    const el = e.target as HTMLElement;
    el.contentEditable = "false";
    const title = el.textContent?.trim() || "Untitled";
    this.dispatchEvent(
      new CustomEvent("sc-session-rename", {
        bubbles: true,
        composed: true,
        detail: { id, title },
      }),
    );
  }

  private _renameKeydown(e: KeyboardEvent, _id: string): void {
    if (e.key === "Enter") {
      e.preventDefault();
      (e.target as HTMLElement).blur();
    }
    if (e.key === "Escape") {
      (e.target as HTMLElement).contentEditable = "false";
      this.requestUpdate();
    }
  }

  override render() {
    const filteredGroups = this._groupSessions(this._filteredSessions);
    let startIndex = 0;
    const groupsWithIndices = filteredGroups.map((g) => {
      const result = { ...g, startIndex };
      startIndex += g.sessions.length;
      return result;
    });

    return html`
      <div class="panel" role="navigation" aria-label="Chat sessions">
        <button type="button" class="new-chat-btn" @click=${this._onNewChat} aria-label="New chat">
          ${icons["file-text"]} New Chat
        </button>
        <div class="search-wrap">
          <input
            class="search-input"
            type="text"
            placeholder="Search sessions..."
            .value=${this._searchQuery}
            @input=${(e: Event) => {
              this._searchQuery = (e.target as HTMLInputElement).value;
              this._focusedIndex = -1;
            }}
            aria-label="Search sessions"
          />
        </div>
        <div
          class="session-list"
          role="listbox"
          tabindex="0"
          aria-label="Session list"
          @keydown=${this._onListKeydown}
        >
          ${filteredGroups.length === 0
            ? this.sessions.length === 0 && !this._searchQuery
              ? html`
                  <div class="empty-sessions">
                    <span class="empty-text">No conversations yet</span>
                    <span class="empty-hint">Start a new chat to begin</span>
                  </div>
                `
              : html`<div class="empty-state">No sessions found</div>`
            : groupsWithIndices.map((group) => {
                return html`
                  <div class="session-group">
                    <span class="group-label">${group.label}</span>
                    ${group.sessions.map((s, si) => {
                      const flatIndex = group.startIndex + si;
                      const isFocused = flatIndex === this._focusedIndex;
                      return html`
                        <div
                          class="session-item ${s.active ? "active" : ""} ${isFocused
                            ? "focused"
                            : ""}"
                          role="option"
                          tabindex="-1"
                          aria-selected=${isFocused}
                          @click=${() => this._onSelect(s.id)}
                          @keydown=${(e: KeyboardEvent) => {
                            if (e.key === "Enter" || e.key === " ") {
                              e.preventDefault();
                              this._onSelect(s.id);
                            }
                          }}
                        >
                          <div class="session-content">
                            <span
                              class="session-title"
                              @dblclick=${(e: Event) => this._startRename(e, s)}
                              @blur=${(e: Event) => this._finishRename(e, s.id)}
                              @keydown=${(e: KeyboardEvent) => this._renameKeydown(e, s.id)}
                              >${s.title || "Untitled"}</span
                            >
                            <span class="session-ts">${formatRelative(s.ts)}</span>
                          </div>
                          <button
                            type="button"
                            class="delete-btn"
                            aria-label="Delete session"
                            @click=${(e: Event) => this._onDelete(e, s.id)}
                          >
                            ${icons.x}
                          </button>
                        </div>
                      `;
                    })}
                  </div>
                `;
              })}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-chat-sessions-panel": ScChatSessionsPanel;
  }
}
