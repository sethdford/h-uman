import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { formatRelative } from "../utils.js";
import "./hu-empty-state.js";

export interface ChatSession {
  id: string;
  title: string;
  ts: number;
  active: boolean;
  projectId?: string;
}

export interface ChatProject {
  id: string;
  name: string;
  instructions?: string;
  pinned?: boolean;
  color?: string;
}

@customElement("hu-chat-sessions-panel")
export class ScChatSessionsPanel extends LitElement {
  @property({ type: Array }) sessions: ChatSession[] = [];
  @property({ type: Array }) projects: ChatProject[] = [];

  @property({ type: Boolean, reflect: true }) open = false;

  @state() private _searchQuery = "";
  @state() private _focusedIndex = -1;
  @state() private _activeProjectFilter: string | null = null;
  @state() private _creatingProject = false;
  @state() private _newProjectName = "";
  @state() private _editingProjectId: string | null = null;

  private get _filteredSessions(): ChatSession[] {
    let sessions = this.sessions;
    if (this._activeProjectFilter) {
      sessions = sessions.filter((s) => s.projectId === this._activeProjectFilter);
    }
    const q = this._searchQuery.toLowerCase();
    if (!q) return sessions;
    return sessions.filter((s) => s.title.toLowerCase().includes(q));
  }

  static override styles = css`
    :host {
      --_panel-width: 16.25rem;
      --_panel-width-expanded: 17.5rem;
      display: block;
      width: 0;
      contain: layout style;
      container-type: inline-size;
      overflow: hidden;
      flex-shrink: 0;
      transition: width var(--hu-duration-normal) var(--hu-ease-spring);
    }

    :host([open]) {
      width: var(--_panel-width);
    }

    .panel {
      width: var(--_panel-width);
      height: 100%;
      display: flex;
      flex-direction: column;
      background: var(--hu-surface-container);
      border-right: 1px solid var(--hu-border-subtle);
      overflow: hidden;
    }

    @media (max-width: 904px) /* --hu-breakpoint-medium */ {
      :host([open]) {
        position: fixed;
        left: 0;
        top: 0;
        bottom: 0;
        z-index: 20;
        width: var(--_panel-width-expanded);
      }
    }

    .panel-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-sm) var(--hu-space-sm) 0;
    }
    .panel-title {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-semibold, 600);
      color: var(--hu-text);
      padding-inline-start: var(--hu-space-sm);
    }
    .panel-close {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 1.75rem;
      height: 1.75rem;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }
    .panel-close:hover {
      color: var(--hu-text);
      background: var(--hu-hover-overlay);
    }
    .panel-close:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .panel-close svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .new-chat-btn {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      box-sizing: border-box;
      width: calc(100% - 2 * var(--hu-space-sm));
      padding: var(--hu-space-sm) var(--hu-space-md);
      margin: var(--hu-space-sm);
      background: transparent;
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
      &:hover {
        background: var(--hu-hover-overlay);
        border-color: var(--hu-accent);
        color: var(--hu-accent-text, var(--hu-accent));
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      & svg {
        width: var(--hu-icon-md);
        height: var(--hu-icon-md);
        flex-shrink: 0;
      }
    }

    .search-wrap {
      padding: 0 var(--hu-space-sm);
      margin-bottom: var(--hu-space-xs);
    }

    .search-input {
      box-sizing: border-box;
      width: 100%;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: var(--hu-bg-inset);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      outline: none;
      transition: border-color var(--hu-duration-fast) var(--hu-ease-out);
      &:focus {
        border-color: var(--hu-accent);
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
      &::placeholder {
        color: var(--hu-text-faint);
      }
    }

    .session-list {
      flex: 1;
      overflow-y: auto;
      padding: 0 var(--hu-space-sm) var(--hu-space-md);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .group-label {
      position: sticky;
      top: 0;
      display: block;
      font-size: var(--hu-text-2xs, 0.625rem);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-secondary);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      padding: var(--hu-space-sm) var(--hu-space-md);
      margin-top: var(--hu-space-xs);
      background: var(--hu-surface-container);
      z-index: 1;
    }

    .session-group:first-child .group-label {
      margin-top: 0;
    }

    .session-item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      border-radius: var(--hu-radius);
      border-left: 3px solid transparent;
      cursor: pointer;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out);
      text-align: left;
      background: transparent;
      border-right: none;
      border-top: none;
      border-bottom: none;
      width: 100%;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      &:hover {
        background: var(--hu-hover-overlay);
      }
      &.active {
        border-left-color: var(--hu-accent-subtle);
        background: var(--hu-surface-container-high);
      }
      &.focused {
        background: var(--hu-surface-container-high);
      }
      &:focus-visible {
        outline: 2px solid var(--hu-accent);
        outline-offset: 2px;
      }
    }

    .session-content {
      flex: 1;
      min-width: 0;
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .session-title {
      font-weight: var(--hu-weight-medium);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .session-ts {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-secondary);
    }

    .delete-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      opacity: 0;
      flex-shrink: 0;
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .session-item:hover .delete-btn {
      opacity: 1;
    }

    .delete-btn:hover {
      color: var(--hu-error);
      background: var(--hu-error-dim);
    }

    .delete-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
      opacity: 1;
    }

    .delete-btn svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .projects-bar {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-bottom: 1px solid var(--hu-border-subtle);
    }
    .project-chip {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }
    .project-chip:hover {
      color: var(--hu-text);
      border-color: var(--hu-border);
    }
    .project-chip.active {
      color: var(--hu-accent-text, var(--hu-accent));
      border-color: color-mix(in srgb, var(--hu-accent) 40%, transparent);
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
    }
    .project-chip:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .project-chip svg {
      width: 0.75rem;
      height: 0.75rem;
    }
    .project-dot {
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: 50%;
      flex-shrink: 0;
    }
    .add-project-chip {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: transparent;
      border: 1px dashed var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast),
        border-color var(--hu-duration-fast);
    }
    .add-project-chip:hover {
      color: var(--hu-accent);
      border-color: var(--hu-accent);
    }
    .add-project-chip:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .add-project-chip svg {
      width: 0.75rem;
      height: 0.75rem;
    }
    .new-project-row {
      display: flex;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
    }
    .new-project-input {
      flex: 1;
      min-width: 0;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: var(--hu-bg-inset);
      border: 1px solid var(--hu-accent);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      outline: none;
    }
    .new-project-input::placeholder {
      color: var(--hu-text-faint);
    }
    .new-project-confirm {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-2xs);
      background: var(--hu-accent);
      color: var(--hu-on-accent);
      border: none;
      border-radius: var(--hu-radius);
      cursor: pointer;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
    }
    .new-project-confirm:hover:not(:disabled) {
      background: var(--hu-accent-hover);
    }
    .new-project-confirm:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }
    .new-project-confirm:active:not(:disabled) {
      transform: scale(0.92);
    }
    .new-project-confirm:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }
    .new-project-confirm svg {
      width: var(--hu-icon-xs);
      height: var(--hu-icon-xs);
    }
    .session-project-indicator {
      display: inline-flex;
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: 50%;
      flex-shrink: 0;
      margin-inline-end: var(--hu-space-2xs);
    }
    @media (prefers-reduced-motion: reduce) {
      :host {
        transition: none;
      }
      .new-chat-btn,
      .session-item,
      .delete-btn,
      .search-input,
      .project-chip,
      .add-project-chip {
        transition: none;
      }
    }
  `;

  private _onClose(): void {
    this.dispatchEvent(
      new CustomEvent("hu-sessions-close", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onNewChat(): void {
    this.dispatchEvent(
      new CustomEvent("hu-session-new", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onSelect(id: string): void {
    this.dispatchEvent(
      new CustomEvent("hu-session-select", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
  }

  private _onDelete(e: Event, id: string): void {
    e.stopPropagation();
    this.dispatchEvent(
      new CustomEvent("hu-session-delete", {
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
      new CustomEvent("hu-session-rename", {
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

  private _toggleProjectFilter(projectId: string): void {
    this._activeProjectFilter = this._activeProjectFilter === projectId ? null : projectId;
    this._focusedIndex = -1;
  }

  private _startCreateProject(): void {
    this._creatingProject = true;
    this._newProjectName = "";
  }

  private _confirmCreateProject(): void {
    const name = this._newProjectName.trim();
    if (!name) return;
    this._creatingProject = false;
    this.dispatchEvent(
      new CustomEvent("hu-project-create", {
        bubbles: true,
        composed: true,
        detail: { name },
      }),
    );
    this._newProjectName = "";
  }

  private _cancelCreateProject(): void {
    this._creatingProject = false;
    this._newProjectName = "";
  }

  private _onProjectKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      e.preventDefault();
      this._confirmCreateProject();
    } else if (e.key === "Escape") {
      this._cancelCreateProject();
    }
  }

  private _getProjectColor(project: ChatProject): string {
    return project.color ?? "var(--hu-accent)";
  }

  private _renderProjectDot(session: ChatSession) {
    if (!session.projectId) return nothing;
    const project = this.projects.find((p) => p.id === session.projectId);
    if (!project) return nothing;
    return html`<span
      class="session-project-indicator"
      style="background: ${this._getProjectColor(project)}"
      title=${project.name}
    ></span>`;
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
        <div class="panel-header">
          <span class="panel-title">Chats</span>
          <button
            type="button"
            class="panel-close"
            @click=${this._onClose}
            aria-label="Close sessions panel"
          >
            ${icons.x}
          </button>
        </div>
        <button type="button" class="new-chat-btn" @click=${this._onNewChat} aria-label="New chat">
          ${icons["file-text"]} New Chat
        </button>
        ${
          this.projects.length > 0 || this._creatingProject
            ? html`
                <div class="projects-bar" role="toolbar" aria-label="Projects">
                  <button
                    class="project-chip ${this._activeProjectFilter === null ? "active" : ""}"
                    type="button"
                    @click=${() => (this._activeProjectFilter = null)}
                  >
                    All
                  </button>
                  ${this.projects.map(
                    (p) => html`
                      <button
                        class="project-chip ${this._activeProjectFilter === p.id ? "active" : ""}"
                        type="button"
                        @click=${() => this._toggleProjectFilter(p.id)}
                        title=${p.instructions ? `Instructions: ${p.instructions}` : p.name}
                      >
                        <span
                          class="project-dot"
                          style="background: ${this._getProjectColor(p)}"
                        ></span>
                        ${p.name} ${p.pinned ? icons["push-pin"] : nothing}
                      </button>
                    `,
                  )}
                  <button
                    class="add-project-chip"
                    type="button"
                    @click=${this._startCreateProject}
                    aria-label="Create project"
                  >
                    ${icons.plus} Project
                  </button>
                </div>
              `
            : html`
                <div class="projects-bar">
                  <button
                    class="add-project-chip"
                    type="button"
                    @click=${this._startCreateProject}
                    aria-label="Create project"
                  >
                    ${icons.plus} New Project
                  </button>
                </div>
              `
        }
        ${
          this._creatingProject
            ? html`
                <div class="new-project-row">
                  <input
                    class="new-project-input"
                    type="text"
                    placeholder="Project name..."
                    .value=${this._newProjectName}
                    @input=${(e: Event) =>
                      (this._newProjectName = (e.target as HTMLInputElement).value)}
                    @keydown=${this._onProjectKeydown}
                    autofocus
                  />
                  <button
                    class="new-project-confirm"
                    type="button"
                    ?disabled=${!this._newProjectName.trim()}
                    @click=${this._confirmCreateProject}
                    aria-label="Create"
                  >
                    ${icons.check}
                  </button>
                </div>
              `
            : nothing
        }
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
          role=${filteredGroups.length > 0 ? "listbox" : "region"}
          tabindex="0"
          aria-label="Session list"
          @keydown=${this._onListKeydown}
        >
          ${
            filteredGroups.length === 0
              ? this.sessions.length === 0 && !this._searchQuery
                ? html`
                    <hu-empty-state
                      heading="No conversations yet"
                      description="Start a new chat to begin."
                      .icon=${icons["chat-circle"] ?? icons["message-square"]}
                    ></hu-empty-state>
                  `
                : html`
                    <hu-empty-state
                      heading="No sessions"
                      description="Start a new chat to begin a session."
                      .icon=${icons["chat-circle"] ?? icons["message-square"]}
                    ></hu-empty-state>
                  `
              : groupsWithIndices.map((group) => {
                  return html`
                    <div class="session-group" role="group" aria-label=${group.label}>
                      <span class="group-label">${group.label}</span>
                      ${group.sessions.map((s, si) => {
                        const flatIndex = group.startIndex + si;
                        const isFocused = flatIndex === this._focusedIndex;
                        return html`
                          <div
                            class="session-item ${s.active ? "active" : ""} ${
                              isFocused ? "focused" : ""
                            }"
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
                                >${this._renderProjectDot(s)}${s.title || "Untitled"}</span
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
                })
          }
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-chat-sessions-panel": ScChatSessionsPanel;
  }
}
