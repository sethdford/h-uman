import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatRelative } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-card.js";
import "../components/hu-dialog.js";
import "../components/hu-empty-state.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-search.js";
import "../components/hu-skeleton.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import { friendlyError } from "../utils/friendly-error.js";

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

@customElement("hu-sessions-view")
export class ScSessionsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-sessions;
      display: block;
      color: var(--hu-text);
      max-width: 75rem;
      padding: var(--hu-space-lg) var(--hu-space-xl);
    }

    .staleness {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .search-row {
      margin-bottom: var(--hu-space-lg);
      max-width: 20rem;
    }

    .sessions-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(18rem, 1fr));
      gap: var(--hu-space-lg);
    }

    .session-card {
      position: relative;
    }

    .session-card-header {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: var(--hu-space-sm);
      margin-bottom: var(--hu-space-sm);
    }

    .session-card-title {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
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
      gap: var(--hu-space-2xs);
    }

    .session-card-preview {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      line-height: var(--hu-leading-relaxed);
      overflow: hidden;
      text-overflow: ellipsis;
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      margin-bottom: var(--hu-space-sm);
    }

    .session-card-meta {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .sessions-grid {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
        transition-duration: 0s !important;
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
      this.error = friendlyError(e);
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
        ? html`<hu-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></hu-empty-state>`
        : this._renderContent()}
      <hu-dialog
        ?open=${this._deleteTarget != null}
        title="Delete session"
        message=${this._deleteTarget
          ? `Are you sure you want to delete "${this.sessionTitle(this._deleteTarget)}"? This cannot be undone.`
          : ""}
        confirmLabel="Delete"
        variant="danger"
        @hu-confirm=${this._onDeleteConfirm}
        @hu-cancel=${this._onDeleteCancel}
      ></hu-dialog>
    `;
  }

  private _renderHero() {
    return html`
      <hu-page-hero role="region" aria-label="Sessions overview">
        <hu-section-header
          heading="Sessions"
          description="Browse and manage your conversation sessions"
        >
          <span class="staleness">${this.stalenessLabel}</span>
          <hu-button size="sm" @click=${() => this.load()} aria-label="Refresh sessions">
            Refresh
          </hu-button>
          <hu-button
            variant="primary"
            size="sm"
            @click=${() => this.dispatchNavigate("chat:default")}
            aria-label="Start new session"
          >
            New Session
          </hu-button>
        </hu-section-header>
      </hu-page-hero>
    `;
  }

  private _renderStats() {
    return html`
      <hu-stats-row>
        <hu-stat-card
          .value=${this.sessions.length}
          label="Sessions"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.activeCount}
          label="Active"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${this.totalMessages}
          label="Total Messages"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
      </hu-stats-row>
    `;
  }

  private _renderSearch() {
    return html`
      <div class="search-row" role="search">
        <hu-search
          .value=${this.searchQuery}
          placeholder="Search sessions..."
          size="md"
          @hu-search=${this._onSearch}
        ></hu-search>
      </div>
    `;
  }

  private _renderContent() {
    const filtered = this.filteredSessions;
    if (filtered.length === 0) {
      return html`
        <hu-empty-state
          .icon=${icons["chat-circle"]}
          heading=${this.searchQuery ? "No matching sessions" : "No sessions yet"}
          description=${this.searchQuery
            ? "Try a different search term."
            : "Start a conversation to create your first session."}
        ></hu-empty-state>
      `;
    }
    return html`
      <div class="sessions-grid" role="list">
        ${filtered.map(
          (s) => html`
            <hu-card
              class="session-card"
              hoverable
              clickable
              @click=${() => this._onSessionClick(s)}
              role="listitem"
            >
              <div class="session-card-header">
                <span class="session-card-title">${this.sessionTitle(s)}</span>
                <div class="session-card-actions">
                  <hu-badge variant=${s.status === "archived" ? "neutral" : "success"}>
                    ${s.status === "archived" ? "Archived" : "Active"}
                  </hu-badge>
                  <hu-button
                    variant="ghost"
                    size="sm"
                    .iconOnly=${true}
                    aria-label="Delete session"
                    @click=${(e: Event) => this._onDeleteClick(e, s)}
                  >
                    ${icons.trash}
                  </hu-button>
                </div>
              </div>
              ${s.last_message
                ? html`<p class="session-card-preview">${s.last_message}</p>`
                : nothing}
              <div class="session-card-meta">
                <span>${this.sessionMessageCount(s)} messages</span>
                <span>${formatRelative(this.sessionTimestamp(s))}</span>
              </div>
            </hu-card>
          `,
        )}
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <hu-page-hero role="region" aria-label="Sessions overview">
        <hu-section-header
          heading="Sessions"
          description="Browse and manage your conversation sessions"
        ></hu-section-header>
      </hu-page-hero>
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <div class="search-row">
        <hu-skeleton variant="line" height="var(--hu-input-min-height)" width="20rem"></hu-skeleton>
      </div>
      <div class="sessions-grid">
        <hu-skeleton variant="session-card"></hu-skeleton>
        <hu-skeleton variant="session-card"></hu-skeleton>
        <hu-skeleton variant="session-card"></hu-skeleton>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-sessions-view": ScSessionsView;
  }
}
