import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
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

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-sessions;
        display: block;
        width: 100%;
        color: var(--hu-text);
        max-width: 75rem;
        contain: layout style;
        container-type: inline-size;
        padding: var(--hu-space-adaptive-page-y) var(--hu-space-adaptive-page-x);
      }

      .staleness {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-secondary);
      }

      .search-row {
        margin-bottom: var(--hu-space-lg);
        max-width: 20rem;
      }

      .sessions-layout {
        display: block;
      }

      .sessions-list-col {
        min-width: 0;
      }

      @media (min-width: 1240px) /* --hu-breakpoint-wide */ {
        .sessions-layout.has-detail {
          display: grid;
          grid-template-columns: 1fr var(--hu-detail-panel-width);
          gap: var(--hu-space-lg);
          align-items: start;
        }
      }

      .sessions-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-lg), 100%), 1fr));
        gap: var(--hu-space-adaptive-content-gap);
      }

      .session-detail {
        padding: var(--hu-space-lg);
        border-radius: var(--hu-radius-xl);
        border: 1px solid var(--hu-border-subtle);
        background: var(--hu-surface-container);
        box-shadow: var(--hu-shadow-card);
      }

      .session-detail-header {
        view-transition-name: hu-selected-item;
        margin: 0 0 var(--hu-space-md);
      }

      .session-detail-title {
        font-size: var(--hu-text-lg);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text);
        margin: 0;
        line-height: var(--hu-leading-tight);
      }

      .session-detail-meta {
        font-size: var(--hu-text-sm);
        color: var(--hu-text-secondary);
        margin-bottom: var(--hu-space-lg);
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-xs);
      }

      .session-detail-actions {
        display: flex;
        flex-wrap: wrap;
        gap: var(--hu-space-sm);
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
        color: var(--hu-text-secondary);
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
        color: var(--hu-text-secondary);
      }
      .session-hula-badge {
        display: inline-flex;
        align-items: center;
        gap: 0.25em;
        color: var(--hu-accent);
        font-weight: 600;
      }
      .session-hula-badge svg {
        width: 1em;
        height: 1em;
      }

      @container (max-width: 48rem) /* cq-medium */ {
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
    `,
  ];

  @state() private sessions: Session[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private searchQuery = "";
  @state() private _deleteTarget: Session | null = null;
  @state() private selectedSession: Session | null = null;
  @state() private _wideListDetail = false;
  private _scrollEntranceObserver: IntersectionObserver | null = null;
  private _wideMq: MediaQueryList | null = null;
  private readonly _wideMqHandler = (): void => {
    this._wideListDetail = this._wideMq?.matches ?? false;
  };

  private get _showSessionDetail(): boolean {
    return this.selectedSession != null && this._wideListDetail;
  }

  override connectedCallback(): void {
    super.connectedCallback();
    if (typeof window !== "undefined" && typeof window.matchMedia === "function") {
      this._wideMq = window.matchMedia("(min-width: 1240px)"); /* --hu-breakpoint-wide */
      this._wideListDetail = this._wideMq.matches;
      this._wideMq.addEventListener("change", this._wideMqHandler);
    }
  }

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => this._setupScrollEntrance());
  }

  override disconnectedCallback(): void {
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    this._wideMq?.removeEventListener("change", this._wideMqHandler);
    this._wideMq = null;
    super.disconnectedCallback();
  }

  private _setupScrollEntrance(): void {
    if (typeof CSS !== "undefined" && CSS.supports?.("animation-timeline", "view()")) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = root.querySelectorAll(".sessions-grid > *");
    if (elements.length === 0) return;
    if (!this._scrollEntranceObserver) {
      this._scrollEntranceObserver = new IntersectionObserver(
        (entries) => {
          entries.forEach((e) => {
            if (e.isIntersecting) {
              (e.target as HTMLElement).classList.add("entered");
              this._scrollEntranceObserver?.unobserve(e.target);
            }
          });
        },
        { threshold: 0.1 },
      );
    }
    elements.forEach((el) => this._scrollEntranceObserver!.observe(el));
  }

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.error = "Not connected to gateway";
      return;
    }
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

  private _prefersReducedMotion(): boolean {
    return (
      typeof window !== "undefined" && window.matchMedia("(prefers-reduced-motion: reduce)").matches
    );
  }

  private async _onSessionClick(s: Session, e: Event): Promise<void> {
    const wide = typeof window !== "undefined" && window.matchMedia("(min-width: 1240px)").matches;
    if (!wide) {
      const key = s.key ?? "default";
      this.dispatchNavigate("chat:" + key);
      return;
    }

    if (this.selectedSession?.key != null && this.selectedSession.key === s.key) {
      return;
    }

    const clicked = e.currentTarget as HTMLElement;
    clicked.style.viewTransitionName = "hu-selected-item";

    const doc = document as Document & {
      startViewTransition?: (cb: () => void | Promise<void>) => {
        finished: Promise<void>;
        ready: Promise<void>;
      };
    };

    const clearVtName = (): void => {
      clicked.style.viewTransitionName = "";
    };

    if (typeof doc.startViewTransition === "function" && !this._prefersReducedMotion()) {
      try {
        const transition = doc.startViewTransition(async () => {
          this.selectedSession = s;
          await this.updateComplete;
          clearVtName();
        });
        /* See app.ts._switchView for rationale: .ready also rejects on skip
         * and must have a handler to prevent unhandled-rejection noise. */
        transition.ready.catch(() => undefined);
        await transition.finished;
      } catch {
        clearVtName();
      } finally {
        clearVtName();
      }
    } else {
      this.selectedSession = s;
      await this.updateComplete;
      clearVtName();
    }
  }

  private _onDeleteClick(e: Event, s: Session): void {
    e.stopPropagation();
    this._deleteTarget = s;
  }

  private _onDeleteConfirm(): void {
    if (!this._deleteTarget) return;
    const key = this._deleteTarget.key;
    const deleted = this._deleteTarget;
    this._deleteTarget = null;
    if (!key) return;
    if (this.selectedSession?.key === deleted.key) {
      this.selectedSession = null;
    }
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
    const showDetail = this._showSessionDetail;
    return html`
      <div class="sessions-layout ${showDetail ? "has-detail" : ""}">
        <div class="sessions-list-col">
          <div class="sessions-grid hu-scroll-reveal-stagger" role="list">
            ${filtered.map(
              (s) => html`
                <hu-card
                  class="session-card"
                  hoverable
                  clickable
                  ?accent=${this.selectedSession?.key === s.key}
                  @click=${(e: Event) => void this._onSessionClick(s, e)}
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
                    ${(s as Record<string, unknown>).hula_count
                      ? html`<span class="session-hula-badge"
                          >${icons.code} ${(s as Record<string, unknown>).hula_count} HuLa</span
                        >`
                      : nothing}
                  </div>
                </hu-card>
              `,
            )}
          </div>
        </div>
        ${this._renderSessionDetail()}
      </div>
    `;
  }

  private _renderSessionDetail() {
    if (!this._showSessionDetail || !this.selectedSession) return nothing;
    const s = this.selectedSession;
    const key = s.key ?? "default";
    return html`
      <section class="session-detail" aria-label="Session detail">
        <header class="session-detail-header">
          <h2 class="session-detail-title">${this.sessionTitle(s)}</h2>
        </header>
        <div class="session-detail-meta">
          <span>${this.sessionMessageCount(s)} messages</span>
          <span>${formatRelative(this.sessionTimestamp(s))}</span>
          <span>Status: ${s.status === "archived" ? "Archived" : "Active"}</span>
          ${(s as Record<string, unknown>).hula_count
            ? html`<span>HuLa Programs: ${(s as Record<string, unknown>).hula_count}</span>`
            : nothing}
        </div>
        <div class="session-detail-actions">
          <hu-button
            variant="primary"
            size="sm"
            @click=${() => this.dispatchNavigate("chat:" + key)}
          >
            Open in chat
          </hu-button>
        </div>
      </section>
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
