import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { formatRelative } from "../utils.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import type { ContextMenuItem } from "../components/sc-context-menu.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-stat-card.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";
import "../components/sc-search.js";
import "../components/sc-context-menu.js";
import "../components/sc-dialog.js";
import "../components/sc-badge.js";
import "../components/sc-message-stream.js";

interface SessionItem {
  key?: string;
  label?: string;
  created_at?: number;
  last_active?: number;
  turn_count?: number;
  preview?: string;
}

interface HistoryMessage {
  role: string;
  content: string;
}

type SortMode = "recent" | "oldest" | "name";
type TimeGroup = "Today" | "Yesterday" | "This Week" | "This Month" | "Older";

const TIME_GROUP_ORDER: TimeGroup[] = ["Today", "Yesterday", "This Week", "This Month", "Older"];

function getTimeGroup(lastActive: number | undefined, now: number): TimeGroup {
  if (lastActive == null) return "Older";
  const ts = lastActive < 1e12 ? lastActive * 1000 : lastActive;
  const diff = now - ts;
  const dayMs = 86_400_000;

  const todayStart = new Date(now);
  todayStart.setHours(0, 0, 0, 0);
  const todayStartMs = todayStart.getTime();

  if (ts >= todayStartMs) return "Today";
  if (ts >= todayStartMs - dayMs) return "Yesterday";
  if (diff < 7 * dayMs) return "This Week";
  if (diff < 30 * dayMs) return "This Month";
  return "Older";
}

function sortSessions(sessions: SessionItem[], mode: SortMode): SessionItem[] {
  const copy = [...sessions];
  switch (mode) {
    case "recent":
      return copy.sort((a, b) => (b.last_active ?? 0) - (a.last_active ?? 0));
    case "oldest":
      return copy.sort((a, b) => (a.last_active ?? 0) - (b.last_active ?? 0));
    case "name":
      return copy.sort((a, b) => {
        const an = (a.label ?? a.key ?? "").toLowerCase();
        const bn = (b.label ?? b.key ?? "").toLowerCase();
        return an.localeCompare(bn);
      });
  }
}

@customElement("sc-sessions-view")
export class ScSessionsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 960px;
    }

    .stats-row {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    @media (max-width: 640px) {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }

    .toolbar {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-md);
    }

    .toolbar sc-search {
      flex: 1;
    }

    .sort-btn {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      cursor: pointer;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out);
      white-space: nowrap;
    }

    .sort-btn:hover {
      color: var(--sc-text);
      border-color: var(--sc-text-faint);
    }

    .sort-btn:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .sort-btn svg {
      width: 1rem;
      height: 1rem;
    }

    .layout {
      display: flex;
      gap: var(--sc-space-xl);
      height: calc(100dvh - 12rem);
    }

    .session-list-panel {
      width: var(--sc-session-list-width, 18rem);
      flex-shrink: 0;
      display: flex;
      flex-direction: column;
      min-height: 0;
    }

    .session-list {
      flex: 1;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .group-label {
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text-faint);
      text-transform: uppercase;
      letter-spacing: var(--sc-tracking-wide, 0.05em);
      padding: var(--sc-space-sm) var(--sc-space-xs);
      position: sticky;
      top: 0;
      background: var(--sc-bg);
      z-index: 1;
    }

    .session-item {
      cursor: pointer;
      border-radius: var(--sc-radius-lg);
      outline: var(--sc-focus-ring-width) solid transparent;
      outline-offset: -2px;
      transition: outline-color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .session-item:focus-visible {
      outline-color: var(--sc-focus-ring);
    }

    .session-item.active {
      outline-color: var(--sc-accent);
    }

    .session-card-inner {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .session-card-top {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: var(--sc-space-sm);
    }

    .session-key {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      flex: 1;
      min-width: 0;
    }

    .session-time {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      white-space: nowrap;
      flex-shrink: 0;
    }

    .session-preview {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      line-height: var(--sc-leading-normal);
    }

    .session-card-bottom {
      display: flex;
      align-items: center;
      justify-content: space-between;
    }

    .detail {
      flex: 1;
      display: flex;
      flex-direction: column;
      min-width: 0;
    }

    .detail-header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-md);
      min-height: 2.5rem;
    }

    .detail-title {
      margin: 0;
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      flex: 1;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .detail-title-editable {
      cursor: text;
      border-radius: var(--sc-radius);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      transition: background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .detail-title-editable:hover {
      background: var(--sc-bg-elevated);
    }

    .rename-input {
      flex: 1;
      min-width: 0;
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: var(--sc-bg);
      border: 1px solid var(--sc-accent);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      font-family: var(--sc-font);
      outline: none;
    }

    .rename-input:focus-visible {
      box-shadow: 0 0 0 var(--sc-focus-ring-width) var(--sc-focus-ring);
    }

    .menu-trigger {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 2rem;
      height: 2rem;
      border: none;
      background: transparent;
      color: var(--sc-text-muted);
      border-radius: var(--sc-radius);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .menu-trigger:hover {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }

    .menu-trigger:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .menu-trigger svg {
      width: 1.25rem;
      height: 1.25rem;
    }

    .history {
      flex: 1;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
    }

    .msg {
      max-width: var(--sc-msg-max-width, 85%);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-base);
      line-height: var(--sc-leading-normal);
      white-space: pre-wrap;
      word-break: break-word;
    }

    .msg.user {
      align-self: flex-end;
      background: var(--sc-accent);
      color: var(--sc-on-accent);
    }

    .msg.assistant {
      align-self: flex-start;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      color: var(--sc-text);
    }

    .resume-bar {
      margin-top: var(--sc-space-md);
      display: flex;
      justify-content: center;
    }

    .empty {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      padding: var(--sc-space-md);
      text-align: center;
    }

    .detail-empty {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .back-btn {
      display: none;
    }

    @media (max-width: 48rem) {
      .layout {
        height: auto;
        min-height: calc(100dvh - 12rem);
      }

      .layout.mobile-detail .session-list-panel {
        display: none;
      }

      .layout:not(.mobile-detail) .detail {
        display: none;
      }

      .session-list-panel {
        width: 100%;
      }

      .back-btn {
        display: flex;
        align-items: center;
        justify-content: center;
        width: 2rem;
        height: 2rem;
        border: none;
        background: transparent;
        color: var(--sc-text-muted);
        border-radius: var(--sc-radius);
        cursor: pointer;
        flex-shrink: 0;
        transition: background var(--sc-duration-fast) var(--sc-ease-out);
      }

      .back-btn:hover {
        background: var(--sc-bg-elevated);
        color: var(--sc-text);
      }

      .back-btn:focus-visible {
        outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      }

      .back-btn svg {
        width: 1.25rem;
        height: 1.25rem;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .session-item,
      .sort-btn,
      .menu-trigger,
      .detail-title-editable {
        transition: none !important;
      }
    }
  `;

  @state() private sessions: SessionItem[] = [];
  @state() private selectedKey = "";
  @state() private messages: HistoryMessage[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private renaming = false;
  @state() private renameValue = "";
  @state() private confirmDeleteOpen = false;
  @state() private searchQuery = "";
  @state() private sortMode: SortMode = "recent";
  @state() private focusedIndex = -1;
  @state() private ctxOpen = false;
  @state() private ctxX = 0;
  @state() private ctxY = 0;
  @state() private ctxKey = "";

  private get filteredSessions(): SessionItem[] {
    const q = this.searchQuery.toLowerCase();
    let list = this.sessions;
    if (q) {
      list = list.filter((s) => (s.label ?? s.key ?? "").toLowerCase().includes(q));
    }
    return sortSessions(list, this.sortMode);
  }

  private groupSessions(sessions: SessionItem[]): Map<TimeGroup, SessionItem[]> {
    if (this.sortMode === "name") {
      const m = new Map<TimeGroup, SessionItem[]>();
      if (sessions.length > 0) m.set("Today", sessions);
      return m;
    }
    const groups = new Map<TimeGroup, SessionItem[]>();
    const now = Date.now();
    for (const s of sessions) {
      const label = getTimeGroup(s.last_active, now);
      if (!groups.has(label)) groups.set(label, []);
      groups.get(label)!.push(s);
    }
    return groups;
  }

  private get contextMenuItems(): ContextMenuItem[] {
    return [
      {
        label: "Resume",
        icon: icons.play as TemplateResult,
        action: () => this.dispatchNavigate("chat:" + (this.ctxKey || "default")),
      },
      { divider: true },
      {
        label: "Rename",
        icon: icons["pencil-simple"] as TemplateResult,
        action: () => {
          this.selectedKey = this.ctxKey;
          this.startRename();
        },
      },
      {
        label: "Delete",
        icon: icons.trash as TemplateResult,
        action: () => {
          this.selectedKey = this.ctxKey;
          this.confirmDeleteOpen = true;
        },
      },
    ];
  }

  protected override async load(): Promise<void> {
    await this.loadSessions();
  }

  private async loadSessions(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const res = await gw.request<{ sessions?: SessionItem[] }>("sessions.list", {});
      this.sessions = res?.sessions ?? [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load sessions";
    } finally {
      this.loading = false;
    }
  }

  private async selectSession(key: string): Promise<void> {
    this.selectedKey = key;
    this.renaming = false;
    this.confirmDeleteOpen = false;
    this.messages = [];
    const gw = this.gateway;
    if (!gw) return;
    try {
      const res = await gw.request<{ messages?: HistoryMessage[] }>("chat.history", {
        sessionKey: key,
      });
      this.messages = res?.messages ?? [];
    } catch {
      this.messages = [];
    }
  }

  private startRename(): void {
    const sess = this.sessions.find((s) => s.key === this.selectedKey);
    this.renameValue = sess?.label ?? sess?.key ?? "";
    this.renaming = true;
    this.updateComplete.then(() => {
      const input = this.renderRoot.querySelector<HTMLInputElement>(".rename-input");
      input?.focus();
      input?.select();
    });
  }

  private async saveRename(): Promise<void> {
    const gw = this.gateway;
    if (!gw || !this.selectedKey) return;
    try {
      await gw.request("sessions.patch", {
        key: this.selectedKey,
        label: this.renameValue.trim(),
      });
      this.renaming = false;
      ScToast.show({ message: "Session renamed", variant: "success" });
      await this.loadSessions();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Rename failed";
      ScToast.show({ message: this.error, variant: "error" });
    }
  }

  private async deleteSession(): Promise<void> {
    const gw = this.gateway;
    const key = this.selectedKey;
    if (!gw || !key) return;
    try {
      await gw.request("sessions.delete", { key });
      this.selectedKey = "";
      this.messages = [];
      this.confirmDeleteOpen = false;
      ScToast.show({ message: "Session deleted", variant: "success" });
      await this.loadSessions();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Delete failed";
      ScToast.show({ message: this.error, variant: "error" });
    }
  }

  private selectedSession(): SessionItem | undefined {
    return this.sessions.find((s) => s.key === this.selectedKey);
  }

  private dispatchNavigate(tab: string): void {
    this.dispatchEvent(
      new CustomEvent("navigate", {
        detail: tab,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private onSearch(e: CustomEvent<{ value: string }>): void {
    this.searchQuery = e.detail.value;
    this.focusedIndex = -1;
  }

  private cycleSortMode(): void {
    const modes: SortMode[] = ["recent", "oldest", "name"];
    const idx = modes.indexOf(this.sortMode);
    this.sortMode = modes[(idx + 1) % modes.length];
  }

  private get sortLabel(): string {
    switch (this.sortMode) {
      case "recent":
        return "Recent";
      case "oldest":
        return "Oldest";
      case "name":
        return "Name A-Z";
    }
  }

  private onListKeyDown(e: KeyboardEvent): void {
    const filtered = this.filteredSessions;
    if (filtered.length === 0) return;

    switch (e.key) {
      case "ArrowDown": {
        e.preventDefault();
        this.focusedIndex = Math.min(this.focusedIndex + 1, filtered.length - 1);
        this.focusSessionItem();
        break;
      }
      case "ArrowUp": {
        e.preventDefault();
        this.focusedIndex = Math.max(this.focusedIndex - 1, 0);
        this.focusSessionItem();
        break;
      }
      case "Enter": {
        e.preventDefault();
        const sess = filtered[this.focusedIndex];
        if (sess?.key) this.selectSession(sess.key);
        break;
      }
      case "Escape": {
        e.preventDefault();
        this.focusedIndex = -1;
        break;
      }
    }
  }

  private focusSessionItem(): void {
    this.updateComplete.then(() => {
      const items = this.renderRoot.querySelectorAll<HTMLElement>('.session-item[role="listitem"]');
      items[this.focusedIndex]?.focus();
    });
  }

  private onContextMenu(e: MouseEvent, key: string): void {
    e.preventDefault();
    this.ctxKey = key;
    this.ctxX = e.clientX;
    this.ctxY = e.clientY;
    this.ctxOpen = true;
  }

  private onRenameKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      e.preventDefault();
      this.saveRename();
    } else if (e.key === "Escape") {
      e.preventDefault();
      this.renaming = false;
    }
  }

  private goBack(): void {
    this.selectedKey = "";
    this.messages = [];
    this.renaming = false;
  }

  override render() {
    const totalSessions = this.sessions.length;
    const totalMessages = this.sessions.reduce((sum, s) => sum + (s.turn_count ?? 0), 0);
    const now = Date.now();
    const todayStart = new Date(now);
    todayStart.setHours(0, 0, 0, 0);
    const todayStartMs = todayStart.getTime();
    const activeSessions = this.sessions.filter((s) => {
      const ts = s.last_active ?? 0;
      const tsMs = ts < 1e12 ? ts * 1000 : ts;
      return tsMs >= todayStartMs;
    }).length;

    return html`
      <sc-page-hero>
        <sc-section-header heading="Sessions" description="Active conversations and their history">
          <sc-button variant="primary" @click=${() => this.dispatchNavigate("chat:default")}>
            New Session
          </sc-button>
          <sc-button variant="secondary" @click=${() => this.loadSessions()}> Refresh </sc-button>
        </sc-section-header>
      </sc-page-hero>
      ${this._renderStats(totalSessions, totalMessages, activeSessions)}
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading ? this._renderSkeleton() : this._renderLayout()}

      <sc-context-menu
        .open=${this.ctxOpen}
        .x=${this.ctxX}
        .y=${this.ctxY}
        .items=${this.contextMenuItems}
        @close=${() => (this.ctxOpen = false)}
      ></sc-context-menu>

      <sc-dialog
        .open=${this.confirmDeleteOpen}
        title="Delete session?"
        message="This action cannot be undone. All messages in this session will be permanently deleted."
        confirmLabel="Delete"
        cancelLabel="Cancel"
        variant="danger"
        @sc-confirm=${() => this.deleteSession()}
        @sc-cancel=${() => (this.confirmDeleteOpen = false)}
      ></sc-dialog>
    `;
  }

  private _renderStats(totalSessions: number, totalMessages: number, activeSessions: number) {
    return html`
      <div class="stats-row">
        <sc-stat-card
          .value=${totalSessions}
          label="Total Sessions"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${totalMessages}
          label="Messages"
          style="--sc-stagger-delay: 80ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${activeSessions}
          label="Active Today"
          style="--sc-stagger-delay: 160ms"
        ></sc-stat-card>
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <div class="layout">
        <div class="session-list-panel">
          <div class="session-list sc-stagger">
            <sc-skeleton variant="session-card"></sc-skeleton>
            <sc-skeleton variant="session-card"></sc-skeleton>
            <sc-skeleton variant="session-card"></sc-skeleton>
            <sc-skeleton variant="session-card"></sc-skeleton>
          </div>
        </div>
        <div class="detail">
          <sc-card class="history-inner">
            <sc-skeleton variant="line" width="80%"></sc-skeleton>
          </sc-card>
        </div>
      </div>
    `;
  }

  private _renderLayout() {
    const filtered = this.filteredSessions;
    const groups = this.groupSessions(filtered);
    const hasMobileDetail = this.selectedKey && this.selectedSession() != null;

    return html`
      <div class="layout ${hasMobileDetail ? "mobile-detail" : ""}">
        <div class="session-list-panel">
          <div class="toolbar">
            <sc-search
              size="sm"
              placeholder="Search sessions..."
              .value=${this.searchQuery}
              @sc-search=${this.onSearch}
            ></sc-search>
            <button
              class="sort-btn"
              @click=${this.cycleSortMode}
              title="Sort: ${this.sortLabel}"
              aria-label="Sort sessions by ${this.sortLabel}"
            >
              ${icons["sort-ascending"]} ${this.sortLabel}
            </button>
          </div>

          ${filtered.length === 0
            ? html`
                <div class="session-list" aria-label="Sessions">
                  <sc-empty-state
                    .icon=${icons["message-square"]}
                    heading=${this.searchQuery ? "No matching sessions" : "No conversations yet"}
                    description=${this.searchQuery
                      ? "Try a different search term."
                      : "Start a chat to see your conversation history here."}
                  ></sc-empty-state>
                </div>
              `
            : html`
                <div
                  class="session-list"
                  role="list"
                  aria-label="Sessions"
                  @keydown=${this.onListKeyDown}
                >
                  ${this.sortMode === "name"
                    ? filtered.map((s, i) => this._renderSessionCard(s, i))
                    : TIME_GROUP_ORDER.filter((g) => groups.has(g)).map(
                        (g) => html`
                          <div role="group" aria-label=${g}>
                            <div class="group-label" role="presentation">${g}</div>
                            ${groups.get(g)!.map((s) => {
                              const idx = filtered.indexOf(s);
                              return this._renderSessionCard(s, idx);
                            })}
                          </div>
                        `,
                      )}
                </div>
              `}
        </div>

        <div class="detail">${this._renderDetail()}</div>
      </div>
    `;
  }

  private _renderSessionCard(s: SessionItem, index: number) {
    const isActive = this.selectedKey === s.key;
    const isFocused = this.focusedIndex === index;
    const preview =
      s.preview ??
      (this.selectedKey === s.key && this.messages.length > 0
        ? this.messages[this.messages.length - 1]?.content?.slice(0, 80)
        : null);

    return html`
      <div
        class="session-item ${isActive ? "active" : ""}"
        role="listitem"
        tabindex=${isFocused || isActive ? "0" : "-1"}
        aria-current=${isActive ? "true" : "false"}
        @click=${() => this.selectSession(s.key ?? "")}
        @contextmenu=${(e: MouseEvent) => this.onContextMenu(e, s.key ?? "")}
      >
        <sc-card hoverable>
          <div class="session-card-inner">
            <div class="session-card-top">
              <span class="session-key">${s.label || s.key || "unnamed"}</span>
              <span class="session-time">${formatRelative(s.last_active)}</span>
            </div>
            ${preview ? html`<div class="session-preview">${preview}</div>` : nothing}
            <div class="session-card-bottom">
              <sc-badge variant="neutral">${s.turn_count ?? 0} turns</sc-badge>
            </div>
          </div>
        </sc-card>
      </div>
    `;
  }

  private _renderDetail() {
    const sel = this.selectedSession();
    if (!sel) {
      return html`
        <div class="detail-empty">
          <div class="empty">Select a session to view history</div>
        </div>
      `;
    }

    return html`
      <div class="detail-header">
        <button class="back-btn" @click=${this.goBack} aria-label="Back to session list">
          ${icons["arrow-left"]}
        </button>

        ${this.renaming
          ? html`
              <input
                class="rename-input"
                type="text"
                .value=${this.renameValue}
                aria-label="Rename session"
                @input=${(e: Event) => (this.renameValue = (e.target as HTMLInputElement).value)}
                @keydown=${this.onRenameKeyDown}
                @blur=${() => this.saveRename()}
              />
            `
          : html`
              <h3
                class="detail-title detail-title-editable"
                @click=${() => this.startRename()}
                title="Click to rename"
              >
                ${sel.label || sel.key}
              </h3>
            `}

        <button
          class="menu-trigger"
          @click=${(e: MouseEvent) => {
            this.ctxKey = this.selectedKey;
            const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
            this.ctxX = rect.left;
            this.ctxY = rect.bottom + 4;
            this.ctxOpen = true;
          }}
          aria-label="Session actions"
        >
          ${icons["dots-three"]}
        </button>
      </div>

      <div class="history" role="log" aria-live="polite" aria-label="Conversation history">
        ${this.messages.length === 0
          ? html`<div class="empty">No messages</div>`
          : this.messages.map((m) =>
              m.role === "assistant"
                ? html`<div class="msg assistant">
                    <sc-message-stream
                      .content=${m.content}
                      .role=${"assistant"}
                    ></sc-message-stream>
                  </div>`
                : html`<div class="msg user">${m.content}</div>`,
            )}
      </div>

      <div class="resume-bar">
        <sc-button
          variant="primary"
          @click=${() => this.dispatchNavigate("chat:" + (this.selectedKey || "default"))}
        >
          Resume conversation
        </sc-button>
      </div>
    `;
  }
}
