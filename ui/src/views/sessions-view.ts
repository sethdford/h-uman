import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatRelative } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-dialog.js";
import "../components/sc-empty-state.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-search.js";
import "../components/sc-skeleton.js";
import "../components/sc-stat-card.js";
import "../components/sc-stats-row.js";

interface Session {
  key?: string;
  label?: string;
  title?: string;
  messages_count?: number;
  turn_count?: number;
  last_message?: string;
  created_at?: number;
  updated_at?: number;
  last_active?: number;
  status?: "active" | "archived";
}

@customElement("sc-sessions-view")
export class ScSessionsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-sessions;
      display: block;
      color: var(--sc-text);
      max-width: 75rem;
      padding: var(--sc-space-lg) var(--sc-space-xl);
    }

    .staleness {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .search-row {
      margin-bottom: var(--sc-space-lg);
      max-width: 20rem;
    }

    .sessions-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(18rem, 1fr));
      gap: var(--sc-space-lg);
    }

    .session-card {
      position: relative;
    }

    .session-card-header {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-sm);
    }

    .session-card-title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      flex: 1;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .session-card-actions {
      flex-shrink: 0;
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
    }

    .session-card-preview {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      line-height: var(--sc-leading-relaxed);
      overflow: hidden;
      text-overflow: ellipsis;
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      margin-bottom: var(--sc-space-sm);
    }

    .session-card-meta {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--sc-space-xs);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    @media (max-width: 48rem) /* --sc-breakpoint-lg */ {
      .sessions-grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private sessions: Session[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private searchQuery = "";
  @state() private _deleteTarget: Session | null = null;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const payload = await gw.request<{ sessions?: Session[] }>("sessions.list", {});
      this.sessions = payload?.sessions ?? [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load sessions";
      this.sessions = [];
    } finally {
      this.loading = false;
    }
  }

  private dispatchNavigate(target: string): void {
    this.dispatchEvent(
      new CustomEvent("navigate", {
        detail: target,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private get totalMessages(): number {
    return this.sessions.reduce((sum, s) => sum + (s.messages_count ?? s.turn_count ?? 0), 0);
  }

  private get activeCount(): number {
    return this.sessions.filter((s) => s.status !== "archived").length;
  }

  private get filteredSessions(): Session[] {
    const q = this.searchQuery.trim().toLowerCase();
    if (!q) return this.sessions;
    return this.sessions.filter((s) => {
      const title = (s.title ?? s.label ?? s.key ?? "").toLowerCase();
      const preview = (s.last_message ?? "").toLowerCase();
      return title.includes(q) || preview.includes(q);
    });
  }

  private sessionTitle(s: Session): string {
    return s.title ?? s.label ?? s.key ?? "Untitled";
  }

  private sessionMessageCount(s: Session): number {
    return s.messages_count ?? s.turn_count ?? 0;
  }

  private sessionTimestamp(s: Session): number | undefined {
    return s.updated_at ?? s.last_active ?? s.created_at;
  }

  private _onSearch(e: CustomEvent<{ value: string }>): void {
    this.searchQuery = e.detail?.value ?? "";
  }

  private _onSessionClick(s: Session): void {
    const key = s.key ?? "default";
    this.dispatchNavigate("chat:" + key);
  }

  private _onDeleteClick(e: Event, s: Session): void {
    e.stopPropagation();
    this._deleteTarget = s;
  }

  private _onDeleteConfirm(): void {
    if (!this._deleteTarget) return;
    const key = this._deleteTarget.key;
    this._deleteTarget = null;
    if (!key) return;
    const gw = this.gateway;
    if (!gw) return;
    gw.request("sessions.delete", { key })
      .then(() => {
        this.sessions = this.sessions.filter((s) => s.key !== key);
      })
      .catch(() => {
        this.error = "Failed to delete session";
      });
  }

  private _onDeleteCancel(): void {
    this._deleteTarget = null;
  }

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      ${this._renderHero()} ${this._renderStats()} ${this._renderSearch()}
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : this._renderContent()}
      <sc-dialog
        ?open=${this._deleteTarget != null}
        title="Delete session"
        message=${this._deleteTarget
          ? `Are you sure you want to delete "${this.sessionTitle(this._deleteTarget)}"? This cannot be undone.`
          : ""}
        confirmLabel="Delete"
        variant="danger"
        @sc-confirm=${this._onDeleteConfirm}
        @sc-cancel=${this._onDeleteCancel}
      ></sc-dialog>
    `;
  }

  private _renderHero() {
    return html`
      <sc-page-hero role="region" aria-label="Sessions overview">
        <sc-section-header
          heading="Sessions"
          description="Browse and manage your conversation sessions"
        >
          <span class="staleness">${this.stalenessLabel}</span>
          <sc-button size="sm" @click=${() => this.load()} aria-label="Refresh sessions">
            Refresh
          </sc-button>
          <sc-button
            variant="primary"
            size="sm"
            @click=${() => this.dispatchNavigate("chat:default")}
            aria-label="Start new session"
          >
            New Session
          </sc-button>
        </sc-section-header>
      </sc-page-hero>
    `;
  }

  private _renderStats() {
    return html`
      <sc-stats-row>
        <sc-stat-card
          .value=${this.sessions.length}
          label="Sessions"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.activeCount}
          label="Active"
          style="--sc-stagger-delay: 50ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${this.totalMessages}
          label="Total Messages"
          style="--sc-stagger-delay: 100ms"
        ></sc-stat-card>
      </sc-stats-row>
    `;
  }

  private _renderSearch() {
    return html`
      <div class="search-row" role="search">
        <sc-search
          .value=${this.searchQuery}
          placeholder="Search sessions..."
          size="md"
          @sc-search=${this._onSearch}
        ></sc-search>
      </div>
    `;
  }

  private _renderContent() {
    const filtered = this.filteredSessions;
    if (filtered.length === 0) {
      return html`
        <sc-empty-state
          .icon=${icons["chat-circle"]}
          heading=${this.searchQuery ? "No matching sessions" : "No sessions yet"}
          description=${this.searchQuery
            ? "Try a different search term."
            : "Start a conversation to create your first session."}
        ></sc-empty-state>
      `;
    }
    return html`
      <div class="sessions-grid" role="list">
        ${filtered.map(
          (s) => html`
            <sc-card
              class="session-card"
              hoverable
              clickable
              @click=${() => this._onSessionClick(s)}
              role="listitem"
            >
              <div class="session-card-header">
                <span class="session-card-title">${this.sessionTitle(s)}</span>
                <div class="session-card-actions">
                  <sc-badge variant=${s.status === "archived" ? "neutral" : "success"}>
                    ${s.status === "archived" ? "Archived" : "Active"}
                  </sc-badge>
                  <sc-button
                    variant="ghost"
                    size="sm"
                    .iconOnly=${true}
                    aria-label="Delete session"
                    @click=${(e: Event) => this._onDeleteClick(e, s)}
                  >
                    ${icons.trash}
                  </sc-button>
                </div>
              </div>
              ${s.last_message
                ? html`<p class="session-card-preview">${s.last_message}</p>`
                : nothing}
              <div class="session-card-meta">
                <span>${this.sessionMessageCount(s)} messages</span>
                <span>${formatRelative(this.sessionTimestamp(s))}</span>
              </div>
            </sc-card>
          `,
        )}
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <sc-page-hero role="region" aria-label="Sessions overview">
        <sc-section-header
          heading="Sessions"
          description="Browse and manage your conversation sessions"
        ></sc-section-header>
      </sc-page-hero>
      <sc-stats-row>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
        <sc-skeleton variant="card" height="90px"></sc-skeleton>
      </sc-stats-row>
      <div class="search-row">
        <sc-skeleton variant="line" height="var(--sc-input-min-height)" width="20rem"></sc-skeleton>
      </div>
      <div class="sessions-grid">
        <sc-skeleton variant="session-card"></sc-skeleton>
        <sc-skeleton variant="session-card"></sc-skeleton>
        <sc-skeleton variant="session-card"></sc-skeleton>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-sessions-view": ScSessionsView;
  }
}
