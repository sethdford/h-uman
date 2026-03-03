import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { formatRelative } from "../utils.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface SessionItem {
  key?: string;
  label?: string;
  created_at?: number;
  last_active?: number;
  turn_count?: number;
}

interface HistoryMessage {
  role: string;
  content: string;
}

@customElement("sc-sessions-view")
export class ScSessionsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-md);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
    }
    .layout {
      display: flex;
      gap: var(--sc-space-md);
      height: calc(100vh - 200px);
    }
    .session-list {
      width: 280px;
      flex-shrink: 0;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .session-item {
      cursor: pointer;
    }
    .session-item:hover {
      --sc-border-subtle: var(--sc-text-muted);
    }
    .session-item.active sc-card {
      border-color: var(--sc-accent);
    }
    .session-key {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-xs);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .session-meta {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
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
      gap: 0.5rem;
      margin-bottom: 1rem;
      flex-wrap: wrap;
    }
    .detail-header h3 {
      margin: 0;
      font-size: 1rem;
      font-weight: 600;
      flex: 1;
    }
    .history {
      flex: 1;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: 0.75rem;
      padding: 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
    }
    .msg {
      max-width: 85%;
      padding: 0.625rem 1rem;
      border-radius: var(--sc-radius);
      font-size: 0.875rem;
      line-height: 1.5;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .msg.user {
      align-self: flex-end;
      background: var(--sc-accent);
      color: var(--sc-bg);
    }
    .msg.assistant {
      align-self: flex-start;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      color: var(--sc-text);
    }
    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-surface) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
      animation: sc-shimmer 1.5s ease-in-out infinite;
      border-radius: var(--sc-radius);
    }
    .skeleton-line {
      height: 1rem;
      margin-bottom: 0.75rem;
      border-radius: 4px;
    }
    .skeleton-card {
      height: 5rem;
      margin-bottom: 0.75rem;
    }
    .empty-state {
      text-align: center;
      padding: 3rem 1rem;
      color: var(--sc-text-muted);
    }
    .empty-icon {
      font-size: 2.5rem;
      margin-bottom: 1rem;
    }
    .empty-title {
      font-size: var(--sc-text-lg);
      font-weight: 600;
      color: var(--sc-text);
      margin: 0 0 0.5rem;
    }
    .empty-desc {
      font-size: var(--sc-text-sm);
      margin: 0;
      max-width: 24rem;
      margin-inline: auto;
    }
    .rename-row {
      display: flex;
      gap: 0.5rem;
      align-items: center;
    }
    .rename-row input {
      padding: 0.375rem 0.75rem;
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: 0.875rem;
      flex: 1;
    }
    .error {
      color: var(--sc-error);
      font-size: 0.875rem;
    }
    @media (max-width: 768px) {
      .layout {
        flex-direction: column;
        height: auto;
      }
      .session-list {
        width: 100%;
        max-height: 200px;
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
  @state() private confirmDelete = false;

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
    this.confirmDelete = false;
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
      await this.loadSessions();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Rename failed";
    }
  }

  private async deleteSession(): Promise<void> {
    const gw = this.gateway;
    if (!gw || !this.selectedKey) return;
    try {
      await gw.request("sessions.delete", { key: this.selectedKey });
      this.selectedKey = "";
      this.messages = [];
      this.confirmDelete = false;
      await this.loadSessions();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Delete failed";
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

  override render() {
    const sel = this.selectedSession();

    return html`
      <div class="header">
        <h2>Sessions</h2>
        <button class="btn" @click=${() => this.loadSessions()}>Refresh</button>
      </div>
      ${this.error ? html`<p class="error">${this.error}</p>` : nothing}
      ${this.loading
        ? html`
            <div class="layout">
              <div class="session-list">
                <div class="session-item skeleton skeleton-card"></div>
                <div class="session-item skeleton skeleton-card"></div>
                <div class="session-item skeleton skeleton-card"></div>
                <div class="session-item skeleton skeleton-card"></div>
              </div>
              <div class="detail">
                <div class="history">
                  <div class="skeleton skeleton-line"></div>
                </div>
              </div>
            </div>
          `
        : html`
            <div class="layout">
              <div class="session-list">
                ${this.sessions.length === 0
                  ? html`
                      <div class="empty-state">
                        <div class="empty-icon">💬</div>
                        <p class="empty-title">No conversations yet</p>
                        <p class="empty-desc">
                          Start a chat to see your conversation history here.
                        </p>
                      </div>
                    `
                  : this.sessions.map(
                      (s) => html`
                        <div
                          class="session-item ${this.selectedKey === s.key ? "active" : ""}"
                          @click=${() => this.selectSession(s.key ?? "")}
                        >
                          <div class="session-key">${s.label || s.key || "unnamed"}</div>
                          <div class="session-meta">
                            ${s.turn_count ?? 0} turns · ${formatRelative(s.last_active)}
                          </div>
                        </div>
                      `,
                    )}
              </div>
              <div class="detail">
                ${sel
                  ? html`
                      <div class="detail-header">
                        ${this.renaming
                          ? html`
                              <div class="rename-row">
                                <input
                                  type="text"
                                  .value=${this.renameValue}
                                  @input=${(e: Event) =>
                                    (this.renameValue = (e.target as HTMLInputElement).value)}
                                />
                                <button class="btn btn-accent" @click=${() => this.saveRename()}>
                                  Save
                                </button>
                                <button class="btn" @click=${() => (this.renaming = false)}>
                                  Cancel
                                </button>
                              </div>
                            `
                          : html`
                              <h3>${sel.label || sel.key}</h3>
                              <button
                                class="btn btn-accent"
                                @click=${() =>
                                  this.dispatchNavigate("chat:" + (this.selectedKey || "default"))}
                              >
                                Resume
                              </button>
                              <button class="btn" @click=${() => this.startRename()}>Rename</button>
                              ${this.confirmDelete
                                ? html`
                                    <button
                                      class="btn-danger btn"
                                      @click=${() => this.deleteSession()}
                                    >
                                      Confirm Delete
                                    </button>
                                    <button
                                      class="btn"
                                      @click=${() => (this.confirmDelete = false)}
                                    >
                                      Cancel
                                    </button>
                                  `
                                : html`
                                    <button
                                      class="btn-danger btn"
                                      @click=${() => (this.confirmDelete = true)}
                                    >
                                      Delete
                                    </button>
                                  `}
                            `}
                      </div>
                      <div class="history">
                        ${this.messages.length === 0
                          ? html`<div class="empty">No messages</div>`
                          : this.messages.map(
                              (m) => html` <div class="msg ${m.role}">${m.content}</div> `,
                            )}
                      </div>
                    `
                  : html`<div class="empty">Select a session to view history</div>`}
              </div>
            </div>
          `}
    `;
  }
}
