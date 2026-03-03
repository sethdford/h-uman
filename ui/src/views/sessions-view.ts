import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { formatRelative } from "../utils.js";
import { icons } from "../icons.js";
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
      max-width: 960px;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
    }
    .layout {
      display: flex;
      gap: var(--sc-space-xl);
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
      border-radius: var(--sc-radius-lg);
      outline: 2px solid transparent;
      outline-offset: -2px;
    }
    .session-item.active {
      outline-color: var(--sc-accent);
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
      font-variant-numeric: tabular-nums;
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
      flex-wrap: wrap;
    }
    .detail-header h3 {
      margin: 0;
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      flex: 1;
    }
    .history {
      flex: 1;
      overflow-y: auto;
    }
    sc-card.history {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .msg {
      max-width: 85%;
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
      color: var(--sc-bg);
    }
    .msg.assistant {
      align-self: flex-start;
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      color: var(--sc-text);
    }
    .rename-row {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .rename-row input {
      padding: var(--sc-space-sm) var(--sc-space-sm);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-base);
      flex: 1;
    }
    .history-inner {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
    }
    .empty {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      padding: var(--sc-space-md);
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
        <sc-button variant="secondary" @click=${() => this.loadSessions()}>Refresh</sc-button>
      </div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading
        ? html`
            <div class="layout">
              <div class="session-list sc-stagger">
                <sc-skeleton variant="card" height="72px"></sc-skeleton>
                <sc-skeleton variant="card" height="72px"></sc-skeleton>
                <sc-skeleton variant="card" height="72px"></sc-skeleton>
                <sc-skeleton variant="card" height="72px"></sc-skeleton>
              </div>
              <div class="detail">
                <sc-card class="history-inner" style="flex: 1; min-height: 120px;">
                  <sc-skeleton variant="line" width="80%"></sc-skeleton>
                </sc-card>
              </div>
            </div>
          `
        : html`
            <div class="layout">
              <div class="session-list sc-stagger">
                ${this.sessions.length === 0
                  ? html`
                      <sc-empty-state
                        .icon=${icons["message-square"]}
                        heading="No conversations yet"
                        description="Start a chat to see your conversation history here."
                      ></sc-empty-state>
                    `
                  : this.sessions.map(
                      (s) => html`
                        <div
                          class="session-item ${this.selectedKey === s.key ? "active" : ""}"
                          @click=${() => this.selectSession(s.key ?? "")}
                        >
                          <sc-card>
                            <div class="session-key">${s.label || s.key || "unnamed"}</div>
                            <div class="session-meta">
                              ${s.turn_count ?? 0} turns · ${formatRelative(s.last_active)}
                            </div>
                          </sc-card>
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
                                <sc-button variant="primary" @click=${() => this.saveRename()}>
                                  Save
                                </sc-button>
                                <sc-button
                                  variant="secondary"
                                  @click=${() => (this.renaming = false)}
                                >
                                  Cancel
                                </sc-button>
                              </div>
                            `
                          : html`
                              <h3>${sel.label || sel.key}</h3>
                              <sc-button
                                variant="primary"
                                @click=${() =>
                                  this.dispatchNavigate("chat:" + (this.selectedKey || "default"))}
                              >
                                Resume
                              </sc-button>
                              <sc-button variant="secondary" @click=${() => this.startRename()}>
                                Rename
                              </sc-button>
                              ${this.confirmDelete
                                ? html`
                                    <sc-button
                                      variant="destructive"
                                      @click=${() => this.deleteSession()}
                                    >
                                      Confirm Delete
                                    </sc-button>
                                    <sc-button
                                      variant="secondary"
                                      @click=${() => (this.confirmDelete = false)}
                                    >
                                      Cancel
                                    </sc-button>
                                  `
                                : html`
                                    <sc-button
                                      variant="destructive"
                                      @click=${() => (this.confirmDelete = true)}
                                    >
                                      Delete
                                    </sc-button>
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
