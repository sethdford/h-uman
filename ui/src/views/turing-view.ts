import { html, css, nothing, type PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { friendlyError } from "../utils/friendly-error.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import "../components/hu-tooltip.js";
import "../components/hu-connection-pulse.js";

interface TuringScore {
  contact_id?: string;
  timestamp?: number;
  overall?: number;
  verdict?: string;
  dimensions?: Record<string, number>;
}

interface TuringScoresRes {
  scores?: TuringScore[];
}

interface TuringTrendRes {
  trend?: Array<{ contact_id?: string; timestamp?: number; overall?: number }>;
}

interface TuringDimensionsRes {
  dimensions?: Record<string, number>;
}

interface TuringTrajectoryRes {
  directional_alignment?: number;
  cumulative_impact?: number;
  stability?: number;
  overall?: number;
}

interface TuringContactRes {
  contact_id?: string;
  dimensions?: Record<string, number>;
  hint?: string | null;
}

interface TuringAbTest {
  name?: string;
  variant_a?: number;
  variant_b?: number;
  avg_a?: number;
  avg_b?: number;
  count_a?: number;
  count_b?: number;
  active?: boolean;
}

interface TuringAbTestsRes {
  tests?: TuringAbTest[];
}

const HU_TURING_DIM_COUNT = 18;

const DIMENSION_ORDER = [
  "natural_language",
  "emotional_intelligence",
  "appropriate_length",
  "personality_consistency",
  "vulnerability_willingness",
  "humor_naturalness",
  "imperfection",
  "opinion_having",
  "energy_matching",
  "context_awareness",
  "non_robotic",
  "genuine_warmth",
  "prosody_naturalness",
  "turn_timing",
  "filler_usage",
  "emotional_prosody",
  "conversational_repair",
  "paralinguistic_cues",
];

function formatDimensionName(key: string): string {
  return key
    .split("_")
    .map((w) => w.charAt(0).toUpperCase() + w.slice(1).toLowerCase())
    .join(" ");
}

function scoreColor(score: number): string {
  if (score >= 8) return "var(--hu-chart-diverging-positive, var(--hu-success))";
  if (score >= 6) return "var(--hu-chart-diverging-neutral, var(--hu-warning))";
  return "var(--hu-chart-diverging-negative, var(--hu-error))";
}

function verdictBadgeVariant(verdict: string): "success" | "warning" | "error" | "neutral" {
  const v = (verdict ?? "").toUpperCase();
  if (v === "HUMAN") return "success";
  if (v === "BORDERLINE") return "warning";
  if (v === "AI_DETECTED") return "error";
  return "neutral";
}

function formatTimestamp(ts: number): string {
  const d = new Date(ts * 1000);
  const now = new Date();
  const diffMs = now.getTime() - d.getTime();
  const diffMins = Math.floor(diffMs / 60000);
  const diffHours = Math.floor(diffMs / 3600000);
  const diffDays = Math.floor(diffMs / 86400000);
  if (diffMins < 1) return "Just now";
  if (diffMins < 60) return `${diffMins}m ago`;
  if (diffHours < 24) return `${diffHours}h ago`;
  if (diffDays < 7) return `${diffDays}d ago`;
  return d.toLocaleDateString();
}

@customElement("hu-turing-view")
export class ScTuringView extends GatewayAwareLitElement {
  override autoRefreshInterval = 60_000;

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-turing;
        display: block;
        width: 100%;
        container-type: inline-size;
        min-width: 0;
        max-width: 75rem;
        contain: layout style;
        padding: var(--hu-space-adaptive-page-y) var(--hu-space-adaptive-page-x);
        font-family: var(--hu-font);
      }

      .hero-score {
        display: flex;
        align-items: baseline;
        gap: var(--hu-space-md);
        flex-wrap: wrap;
      }

      .hero-score-value {
        font-size: var(--hu-text-4xl);
        font-weight: var(--hu-weight-bold);
        color: var(--hu-text);
        line-height: var(--hu-leading-tight);
      }

      .hero-score-max {
        font-size: var(--hu-text-xl);
        font-weight: var(--hu-weight-medium);
        color: var(--hu-text-muted);
      }

      .hero-subtitle {
        font-size: var(--hu-text-sm);
        color: var(--hu-text-muted);
        margin-top: var(--hu-space-xs);
      }

      .staleness {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
      }

      .dimensions-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-md), 100%), 1fr));
        gap: var(--hu-space-adaptive-content-gap);
        margin-bottom: var(--hu-space-2xl);
      }

      .dimension-card {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-sm);
      }

      .dimension-name {
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-medium);
        color: var(--hu-text);
      }

      .dimension-bar-wrap {
        height: var(--hu-space-md);
        background: var(--hu-bg-inset);
        border-radius: var(--hu-radius-sm);
        overflow: hidden;
      }

      .dimension-bar-fill {
        height: 100%;
        border-radius: var(--hu-radius-sm);
        transition: width var(--hu-duration-normal) var(--hu-ease-out);
      }

      .dimension-score {
        font-size: var(--hu-text-xs);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text);
        font-variant-numeric: tabular-nums;
      }

      .section-label {
        font-size: var(--hu-text-xs);
        font-weight: var(--hu-weight-semibold);
        letter-spacing: var(--hu-tracking-xs);
        text-transform: uppercase;
        color: var(--hu-text-secondary);
        margin-bottom: var(--hu-space-md);
      }

      .scores-table {
        width: 100%;
        border-collapse: collapse;
        font-size: var(--hu-text-sm);
      }

      .scores-table th,
      .scores-table td {
        padding: var(--hu-space-sm) var(--hu-space-md);
        text-align: left;
        border-bottom: 1px solid var(--hu-border-subtle);
      }

      .scores-table th {
        font-weight: var(--hu-weight-medium);
        color: var(--hu-text-muted);
      }

      .scores-table td {
        color: var(--hu-text);
      }

      .scores-table tr:last-child td {
        border-bottom: none;
      }

      .trajectory-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-sm), 100%), 1fr));
        gap: var(--hu-space-adaptive-content-gap);
        margin-bottom: var(--hu-space-2xl);
      }

      @container (max-width: 30rem) /* cq-sm */ {
        .dimensions-grid,
        .trajectory-grid {
          grid-template-columns: 1fr;
        }
      }

      .trajectory-card {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: var(--hu-space-xs);
        text-align: center;
      }

      .trajectory-label {
        font-size: var(--hu-text-xs);
        font-weight: var(--hu-weight-medium);
        color: var(--hu-text-muted);
        text-transform: uppercase;
        letter-spacing: var(--hu-tracking-xs);
        white-space: nowrap;
      }

      .trajectory-value {
        font-size: var(--hu-text-2xl);
        font-weight: var(--hu-weight-bold);
        color: var(--hu-text);
        font-variant-numeric: tabular-nums;
      }

      .contact-link {
        background: none;
        border: none;
        color: var(--hu-accent);
        cursor: pointer;
        font: inherit;
        padding: 0;
        text-decoration: underline;
        text-decoration-color: color-mix(in srgb, var(--hu-accent) 40%, transparent);
        transition: text-decoration-color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .contact-link:hover {
        text-decoration-color: var(--hu-accent);
      }

      .contact-hint {
        display: flex;
        align-items: flex-start;
        gap: var(--hu-space-sm);
        padding: var(--hu-space-md);
        margin-bottom: var(--hu-space-md);
        background: color-mix(in srgb, var(--hu-accent-secondary) 10%, transparent);
        border-radius: var(--hu-radius-md);
        font-size: var(--hu-text-sm);
        color: var(--hu-text);
        line-height: var(--hu-leading-relaxed);
      }

      .contact-hint svg {
        flex-shrink: 0;
        width: 1.25rem;
        height: 1.25rem;
        color: var(--hu-accent-secondary);
      }

      .contact-dims {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-xs);
        margin-bottom: var(--hu-space-md);
      }

      .contact-dim-row {
        display: flex;
        justify-content: space-between;
        padding: var(--hu-space-xs) 0;
        border-bottom: 1px solid var(--hu-border-subtle);
        font-size: var(--hu-text-sm);
      }

      .contact-dim-name {
        color: var(--hu-text);
      }

      .contact-dim-score {
        font-weight: var(--hu-weight-semibold);
        font-variant-numeric: tabular-nums;
      }

      @media (prefers-reduced-motion: reduce) {
        .dimension-bar-fill {
          transition: none;
        }
      }
    `,
  ];

  @state() private scores: TuringScore[] = [];
  @state() private dimensions: Record<string, number> = {};
  @state() private loading = true;
  @state() private error = "";
  @state() private connectionStatus: "connected" | "connecting" | "disconnected" = "disconnected";
  private _scrollEntranceObserver: IntersectionObserver | null = null;

  private _connectionStatusHandler = ((e: CustomEvent<string>) => {
    const s = e.detail as "connected" | "connecting" | "disconnected";
    if (s) this.connectionStatus = s;
  }) as EventListener;

  override connectedCallback(): void {
    super.connectedCallback();
    const gw = this.gateway;
    if (gw) {
      gw.addEventListener("status", this._connectionStatusHandler);
      this.connectionStatus = gw.status;
    }
  }

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => this._setupScrollEntrance());
  }

  override disconnectedCallback(): void {
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    super.disconnectedCallback();
    this.gateway?.removeEventListener("status", this._connectionStatusHandler);
  }

  private _setupScrollEntrance(): void {
    if (typeof CSS !== "undefined" && CSS.supports?.("animation-timeline", "view()")) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = root.querySelectorAll(".hu-scroll-reveal-stagger > *");
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

  protected override onGatewaySwapped(
    previous: GatewayAwareLitElement["gateway"],
    current: NonNullable<GatewayAwareLitElement["gateway"]>,
  ): void {
    previous?.removeEventListener("status", this._connectionStatusHandler);
    current.addEventListener("status", this._connectionStatusHandler);
    this.connectionStatus = current.status;
  }

  @state() private trajectory: TuringTrajectoryRes | null = null;
  @state() private abTests: TuringAbTest[] = [];
  @state() private selectedContact: TuringContactRes | null = null;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.loading = false;
      this.error = "Not connected";
      return;
    }
    this.loading = true;
    this.error = "";
    try {
      const [scoresRes, trendRes, dimRes, trajRes, abRes] = await Promise.all([
        gw.request<TuringScoresRes>("turing.scores", {}).catch(() => ({ scores: [] })),
        gw.request<TuringTrendRes>("turing.trend", {}).catch(() => ({ trend: [] })),
        gw.request<TuringDimensionsRes>("turing.dimensions", {}).catch(() => ({ dimensions: {} })),
        gw
          .request<TuringTrajectoryRes>("turing.trajectory", {})
          .catch(() => null as TuringTrajectoryRes | null),
        gw.request<TuringAbTestsRes>("turing.ab_tests", {}).catch(() => ({ tests: [] })),
      ]);

      const scoresPayload = scoresRes as TuringScoresRes;
      this.scores = Array.isArray(scoresPayload?.scores) ? scoresPayload.scores : [];

      const dimPayload = dimRes as TuringDimensionsRes;
      this.dimensions =
        dimPayload?.dimensions && typeof dimPayload.dimensions === "object"
          ? dimPayload.dimensions
          : {};

      this.trajectory = trajRes as TuringTrajectoryRes | null;

      const abPayload = abRes as TuringAbTestsRes;
      this.abTests = Array.isArray(abPayload?.tests) ? abPayload.tests : [];

      if (this.scores.length === 0 && Array.isArray((trendRes as TuringTrendRes)?.trend)) {
        const trend = (trendRes as TuringTrendRes).trend ?? [];
        this.scores = trend.map((t) => ({
          contact_id: t.contact_id ?? "—",
          timestamp: t.timestamp ?? 0,
          overall: t.overall ?? 0,
          verdict: "—",
        }));
      }
    } catch (e) {
      this.error = friendlyError(e);
      this.scores = [];
      this.dimensions = {};
    } finally {
      this.loading = false;
    }
  }

  private async _loadContact(contactId: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    try {
      const res = await gw.request<TuringContactRes>("turing.contact", {
        contact_id: contactId,
      });
      this.selectedContact = res as TuringContactRes;
    } catch {
      this.selectedContact = null;
    }
  }

  private get _overallScore(): number {
    if (this.scores.length > 0 && this.scores[0]?.overall != null) {
      return this.scores[0].overall;
    }
    const vals = Object.values(this.dimensions);
    if (vals.length === 0) return 0;
    return Math.round(vals.reduce((a, b) => a + b, 0) / vals.length);
  }

  private get _verdict(): string {
    return this.scores[0]?.verdict ?? "—";
  }

  private get _conversationCount(): number {
    const ids = new Set(this.scores.map((s) => s.contact_id ?? ""));
    return Math.max(ids.size, this.scores.length);
  }

  private get _recentScores(): TuringScore[] {
    return [...this.scores].sort((a, b) => (b.timestamp ?? 0) - (a.timestamp ?? 0)).slice(0, 20);
  }

  private get _orderedDimensions(): Array<{ key: string; score: number }> {
    const out: Array<{ key: string; score: number }> = [];
    for (const key of DIMENSION_ORDER) {
      const score = this.dimensions[key];
      if (score != null) out.push({ key, score });
    }
    for (const [key, score] of Object.entries(this.dimensions)) {
      if (!DIMENSION_ORDER.includes(key)) out.push({ key, score });
    }
    return out;
  }

  override render() {
    if (this.loading)
      return html`<div class="hu-scroll-reveal-stagger">${this._renderSkeleton()}</div>`;
    if (this.error)
      return html`
        <div class="hu-scroll-reveal-stagger">
          <hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <hu-button variant="primary" @click=${() => this.load()}>Retry</hu-button>
          </hu-empty-state>
        </div>
      `;

    const hasData = this.scores.length > 0 || Object.keys(this.dimensions).length > 0;
    if (!hasData)
      return html`
        <div class="hu-scroll-reveal-stagger">
          <hu-page-hero role="region" aria-label="Turing Dashboard">
            <hu-section-header
              heading="Turing Dashboard"
              description="Human Fidelity Score across conversations"
            >
              <hu-connection-pulse status=${this.connectionStatus}></hu-connection-pulse>
              <hu-tooltip text="Reload Turing data" position="bottom">
                <hu-button variant="secondary" @click=${() => this.load()}>Refresh</hu-button>
              </hu-tooltip>
            </hu-section-header>
            <hu-empty-state
              .icon=${icons.brain}
              heading="No scores yet"
              description="Turing scores will appear here once conversations are evaluated for human-likeness."
            >
              <hu-button variant="primary" @click=${() => this.load()}>Refresh</hu-button>
            </hu-empty-state>
          </hu-page-hero>
        </div>
      `;

    return html`
      <div class="hu-scroll-reveal-stagger">
        ${this._renderHero()} ${this._renderTrajectory()} ${this._renderDimensions()}
        ${this._renderRecentScores()} ${this._renderContactDetail()} ${this._renderAbTests()}
      </div>
    `;
  }

  private _renderHero() {
    const score = this._overallScore;
    const verdict = this._verdict;
    const n = this._conversationCount;

    return html`
      <hu-page-hero role="region" aria-label="Turing Dashboard">
        <hu-section-header
          heading="Turing Dashboard"
          description="Human Fidelity Score across conversations"
        >
          <hu-connection-pulse status=${this.connectionStatus}></hu-connection-pulse>
          ${
            this.lastLoadedAt
              ? html`<span class="staleness">Updated ${this.stalenessLabel}</span>`
              : nothing
          }
          <hu-tooltip text="Reload Turing data" position="bottom">
            <hu-button variant="secondary" @click=${() => this.load()}>Refresh</hu-button>
          </hu-tooltip>
        </hu-section-header>
        <div class="hero-score">
          <span class="hero-score-value" aria-label="Overall score">${score}</span>
          <span class="hero-score-max">/ 10</span>
          <hu-badge variant=${verdictBadgeVariant(verdict)}>${verdict}</hu-badge>
        </div>
        <p class="hero-subtitle">
          Based on ${HU_TURING_DIM_COUNT} humanness dimensions across ${n}
          conversation${n === 1 ? "" : "s"}
        </p>
      </hu-page-hero>
    `;
  }

  private _renderDimensions() {
    const dims = this._orderedDimensions;
    if (dims.length === 0) return nothing;

    return html`
      <div class="section-label">Dimension Scores</div>
      <div class="dimensions-grid">
        ${dims.map(
          (d) => html`
            <hu-card glass surface="high" class="dimension-card">
              <span class="dimension-name">${formatDimensionName(d.key)}</span>
              <div class="dimension-bar-wrap">
                <div
                  class="dimension-bar-fill"
                  style="width: ${(d.score / 10) * 100}%; background: ${scoreColor(d.score)}"
                ></div>
              </div>
              <span class="dimension-score">${d.score}/10</span>
            </hu-card>
          `,
        )}
      </div>
    `;
  }

  private _renderRecentScores() {
    const recent = this._recentScores;
    if (recent.length === 0) return nothing;

    return html`
      <hu-card glass surface="high">
        <div class="section-label">Recent Scores</div>
        <table class="scores-table" role="grid" aria-label="Recent Turing scores">
          <thead>
            <tr>
              <th>Contact</th>
              <th>Time</th>
              <th>Score</th>
              <th>Verdict</th>
            </tr>
          </thead>
          <tbody>
            ${recent.map(
              (s) => html`
                <tr>
                  <td>
                    <button
                      class="contact-link"
                      @click=${() => s.contact_id && this._loadContact(s.contact_id)}
                    >
                      ${s.contact_id ?? "—"}
                    </button>
                  </td>
                  <td>${formatTimestamp(s.timestamp ?? 0)}</td>
                  <td>${s.overall ?? "—"}/10</td>
                  <td>
                    <hu-badge variant=${verdictBadgeVariant(s.verdict ?? "")}>
                      ${s.verdict ?? "—"}
                    </hu-badge>
                  </td>
                </tr>
              `,
            )}
          </tbody>
        </table>
      </hu-card>
    `;
  }

  private _renderTrajectory() {
    const t = this.trajectory;
    if (!t || t.overall == null) return nothing;
    const pct = (v: number | undefined) => (v != null ? `${Math.round(v * 100)}%` : "—");
    return html`
      <div class="section-label">Trajectory</div>
      <div class="trajectory-grid">
        <hu-card glass surface="high" class="trajectory-card">
          <span class="trajectory-label">Direction</span>
          <span class="trajectory-value">${pct(t.directional_alignment)}</span>
        </hu-card>
        <hu-card glass surface="high" class="trajectory-card">
          <span class="trajectory-label">Impact</span>
          <span class="trajectory-value">${pct(t.cumulative_impact)}</span>
        </hu-card>
        <hu-card glass surface="high" class="trajectory-card">
          <span class="trajectory-label">Stability</span>
          <span class="trajectory-value">${pct(t.stability)}</span>
        </hu-card>
        <hu-card glass surface="high" class="trajectory-card">
          <span class="trajectory-label">Overall</span>
          <span class="trajectory-value">${pct(t.overall)}</span>
        </hu-card>
      </div>
    `;
  }

  private _renderContactDetail() {
    const c = this.selectedContact;
    if (!c) return nothing;
    const dims = c.dimensions ?? {};
    const entries = Object.entries(dims).sort(([, a], [, b]) => a - b);
    return html`
      <div class="section-label">Contact: ${c.contact_id ?? "—"}</div>
      <hu-card glass surface="high">
        ${
          c.hint
            ? html`<div class="contact-hint">
                ${icons.lightbulb}
                <span>${c.hint}</span>
              </div>`
            : nothing
        }
        <div class="contact-dims">
          ${entries.map(
            ([key, score]) => html`
              <div class="contact-dim-row">
                <span class="contact-dim-name">${formatDimensionName(key)}</span>
                <span class="contact-dim-score" style="color: ${scoreColor(score)}"
                  >${score}/10</span
                >
              </div>
            `,
          )}
        </div>
        <hu-button variant="secondary" @click=${() => (this.selectedContact = null)}
          >Close</hu-button
        >
      </hu-card>
    `;
  }

  private _renderAbTests() {
    if (this.abTests.length === 0) return nothing;
    return html`
      <div class="section-label">A/B Experiments</div>
      <hu-card glass surface="high">
        <table class="scores-table" role="grid" aria-label="A/B test results">
          <thead>
            <tr>
              <th>Experiment</th>
              <th>Variant A</th>
              <th>Variant B</th>
              <th>Avg A</th>
              <th>Avg B</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
            ${this.abTests.map(
              (t) => html`
                <tr>
                  <td>${formatDimensionName(t.name ?? "")}</td>
                  <td>${t.variant_a?.toFixed(2) ?? "—"}</td>
                  <td>${t.variant_b?.toFixed(2) ?? "—"}</td>
                  <td style="color: ${scoreColor(t.avg_a ?? 0)}">${t.avg_a?.toFixed(1) ?? "—"}</td>
                  <td style="color: ${scoreColor(t.avg_b ?? 0)}">${t.avg_b?.toFixed(1) ?? "—"}</td>
                  <td>
                    <hu-badge variant=${t.active ? "success" : "neutral"}>
                      ${t.active ? "Active" : "Resolved"}
                    </hu-badge>
                  </td>
                </tr>
              `,
            )}
          </tbody>
        </table>
      </hu-card>
    `;
  }

  private _renderSkeleton() {
    return html`
      <hu-page-hero role="region" aria-label="Turing Dashboard">
        <hu-section-header heading="Turing Dashboard" description="Loading…"></hu-section-header>
        <div class="hero-score">
          <hu-skeleton variant="line" width="4rem" height="2.5rem"></hu-skeleton>
          <hu-skeleton variant="line" width="3rem" height="1.25rem"></hu-skeleton>
        </div>
      </hu-page-hero>
      <div class="section-label">Dimension Scores</div>
      <div class="dimensions-grid">
        ${Array.from(
          { length: HU_TURING_DIM_COUNT },
          () => html` <hu-skeleton variant="card" height="6rem"></hu-skeleton> `,
        )}
      </div>
      <hu-skeleton variant="card" height="12rem"></hu-skeleton>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-turing-view": ScTuringView;
  }
}
