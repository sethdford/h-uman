import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-card.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import "../components/hu-fidelity-tile.js";
import type { FidelityStatus } from "../components/hu-fidelity-tile.js";
import "../components/hu-directive-telemetry-tile.js";
import type { DirectiveTelemetry } from "../components/hu-directive-telemetry-tile.js";
import { icons } from "../icons.js";
import { friendlyError } from "../utils/friendly-error.js";

interface MetricsSnapshot {
  health?: { uptime_seconds?: number; pid?: number };
  intelligence?: {
    tree_of_thought?: boolean;
    constitutional_ai?: boolean;
    llm_compiler?: boolean;
    mcts_planner?: boolean;
    speculative_cache?: boolean;
  };
  metrics?: {
    total_requests?: number;
    total_tokens?: number;
    total_tool_calls?: number;
    total_errors?: number;
    avg_latency_ms?: number;
    active_sessions?: number;
  };
  bth?: Record<string, number>;
}

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const parts: string[] = [];
  if (d > 0) parts.push(`${d}d`);
  if (h > 0) parts.push(`${h}h`);
  parts.push(`${m}m`);
  return parts.join(" ");
}

const BTH_GROUPS: Record<string, string[]> = {
  Memory: [
    "emotions_surfaced",
    "facts_extracted",
    "emotions_promoted",
    "replay_analyses",
    "egraph_contexts",
  ],
  Conversation: [
    "pattern_insights",
    "mood_contexts_built",
    "thinking_responses",
    "ab_evaluations",
    "ab_alternates_chosen",
  ],
  Engagement: [
    "silence_checkins",
    "event_followups",
    "starters_built",
    "callbacks_triggered",
    "reactions_sent",
  ],
  Context: [
    "link_contexts",
    "attachment_contexts",
    "vision_descriptions",
    "commitment_followups",
    "events_extracted",
  ],
  Polish: ["typos_applied", "corrections_sent"],
  Cognition: [
    "cognition_fast_turns",
    "cognition_slow_turns",
    "cognition_emotional_turns",
    "metacog_interventions",
    "metacog_regens",
    "metacog_difficulty_easy",
    "metacog_difficulty_medium",
    "metacog_difficulty_hard",
    "metacog_hysteresis_suppressed",
    "hula_tool_turns",
    "episodic_patterns_stored",
    "episodic_replays",
    "skill_routes_semantic",
    "skill_routes_blended",
    "skill_routes_embedded",
    "evolving_outcomes",
  ],
};

function labelFromKey(key: string): string {
  return key.replace(/_/g, " ").replace(/\b\w/g, (c) => c.toUpperCase());
}

@customElement("hu-metrics-view")
export class ScMetricsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 10_000;
  private _scrollEntranceObserver: IntersectionObserver | null = null;

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-metrics;
        display: block;
        width: 100%;
        min-width: 0;
        color: var(--hu-text);
        max-width: 60rem;
        container-type: inline-size;
      }
      .section {
        margin-bottom: var(--hu-space-2xl);
      }
      .hula-cta {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-md);
        align-items: flex-start;
      }
      .metric-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-sm), 100%), 1fr));
        gap: var(--hu-space-md);
      }
      .metric-item {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-2xs);
      }
      .metric-label {
        font-size: var(--hu-text-xs);
        color: var(--hu-text-muted);
        text-transform: uppercase;
        letter-spacing: 0.04em;
      }
      .metric-value {
        font-family: var(--hu-font-mono);
        font-size: var(--hu-text-base);
        font-weight: var(--hu-weight-semibold);
        color: var(--hu-text);
        font-variant-numeric: tabular-nums;
      }
      .bth-group {
        margin-bottom: var(--hu-space-xl);
      }
      .bth-group:last-child {
        margin-bottom: 0;
      }
      .card-inner {
        padding: var(--hu-space-md);
      }
      .intel-badges {
        display: flex;
        flex-wrap: wrap;
        gap: var(--hu-space-sm);
        align-items: center;
      }
      .intel-badge {
        display: inline-flex;
        align-items: center;
        gap: var(--hu-space-2xs);
        padding: var(--hu-space-2xs) var(--hu-space-sm);
        border-radius: var(--hu-radius);
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-medium);
      }
      .intel-badge.enabled {
        background: color-mix(in srgb, var(--hu-accent) 15%, transparent);
        color: var(--hu-accent);
      }
      .intel-badge.enabled svg {
        color: var(--hu-accent);
      }
      .intel-badge.disabled {
        background: var(--hu-surface-dim);
        color: var(--hu-text-muted);
      }
      .intel-badge.disabled svg {
        color: var(--hu-text-muted);
      }

      @container (max-width: 48rem) /* cq-medium */ {
        .metric-grid {
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

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => this._setupScrollEntrance());
  }

  override disconnectedCallback(): void {
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    super.disconnectedCallback();
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

  @state() private snapshot: MetricsSnapshot = {};
  @state() private loading = false;
  @state() private error = "";
  /* Persona fidelity is a separate gateway call so a slow or
   * failing fidelity backend never blocks the main `metrics.snapshot`
   * paint. Both load in parallel; each renders its own loading
   * + error state independently. */
  @state() private fidelity: FidelityStatus | null = null;
  @state() private fidelityError = "";
  /* Same fail-isolation pattern as fidelity: directive telemetry
   * loads in parallel with `metrics.snapshot` so a slow or absent
   * counter backend never blanks the main view. */
  @state() private directiveTelemetry: DirectiveTelemetry | null = null;
  @state() private directiveTelemetryError = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) {
      this.error = "Not connected to gateway";
      return;
    }
    this.loading = true;
    this.error = "";
    try {
      const res = (await gw.request<MetricsSnapshot>("metrics.snapshot", {})) as
        | MetricsSnapshot
        | { result?: MetricsSnapshot };
      this.snapshot =
        (res && "result" in res && res.result) ||
        (res && "health" in res ? (res as MetricsSnapshot) : {}) ||
        {};
    } catch (e) {
      this.error = friendlyError(e);
      this.snapshot = {};
    } finally {
      this.loading = false;
    }
    void this._loadFidelity();
    void this._loadDirectiveTelemetry();
  }

  private async _loadDirectiveTelemetry(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.directiveTelemetry = null;
    this.directiveTelemetryError = "";
    try {
      const res = (await gw.request<DirectiveTelemetry>(
        "metrics.directive_telemetry",
        {},
      )) as DirectiveTelemetry | { result?: DirectiveTelemetry };
      const data =
        (res && "result" in res && (res as { result?: DirectiveTelemetry }).result) ||
        (res && "variants" in res ? (res as DirectiveTelemetry) : null);
      if (data && data.variants !== undefined) {
        this.directiveTelemetry = data;
      } else {
        this.directiveTelemetryError = "no telemetry data";
      }
    } catch (e) {
      this.directiveTelemetryError = friendlyError(e);
    }
  }

  private async _loadFidelity(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.fidelity = null;
    this.fidelityError = "";
    try {
      const res = (await gw.request<FidelityStatus>("metrics.fidelity", {})) as
        | FidelityStatus
        | { result?: FidelityStatus };
      const data =
        (res && "result" in res && (res as { result?: FidelityStatus }).result) ||
        (res && "baseline" in res ? (res as FidelityStatus) : null);
      if (data && data.persona !== undefined) {
        this.fidelity = data;
      } else {
        this.fidelityError = "no fidelity data";
      }
    } catch (e) {
      this.fidelityError = friendlyError(e);
    }
  }

  private _renderSkeleton() {
    return html`
      <hu-stats-row>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
        <hu-skeleton variant="card" height="90px"></hu-skeleton>
      </hu-stats-row>
      <hu-skeleton variant="card" height="180px"></hu-skeleton>
      <hu-skeleton variant="card" height="280px"></hu-skeleton>
    `;
  }

  private _renderSystemHealth() {
    const m = this.snapshot.metrics;
    if (!m) return nothing;

    const items = [
      { key: "total_requests", label: "Total Requests", value: m.total_requests },
      { key: "total_tokens", label: "Total Tokens", value: m.total_tokens },
      { key: "total_tool_calls", label: "Tool Calls", value: m.total_tool_calls },
      { key: "total_errors", label: "Errors", value: m.total_errors },
      { key: "avg_latency_ms", label: "Avg Latency (ms)", value: m.avg_latency_ms },
      { key: "active_sessions", label: "Active Sessions", value: m.active_sessions },
    ];

    return html`
      <div class="section hu-scroll-reveal" role="region" aria-label="System health metrics">
        <hu-section-header
          heading="System Health"
          description="Gateway and request metrics"
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner">
            <div class="metric-grid hu-scroll-reveal-stagger">
              ${items.map(
                (item) => html`
                  <div class="metric-item">
                    <span class="metric-label">${item.label}</span>
                    <span class="metric-value"
                      >${item.value != null
                        ? typeof item.value === "number" && item.value >= 1000
                          ? (item.value as number).toLocaleString()
                          : String(item.value)
                        : "—"}</span
                    >
                  </div>
                `,
              )}
            </div>
          </div>
        </hu-card>
      </div>
    `;
  }

  private _renderIntelligenceStats() {
    const intel = this.snapshot.intelligence;
    if (!intel) return nothing;

    const modules: { key: keyof typeof intel; label: string }[] = [
      { key: "tree_of_thought", label: "Tree of Thought" },
      { key: "constitutional_ai", label: "Constitutional AI" },
      { key: "llm_compiler", label: "LLM Compiler" },
      { key: "mcts_planner", label: "MCTS Planner" },
      { key: "speculative_cache", label: "Speculative Cache" },
    ];

    return html`
      <div class="section hu-scroll-reveal" role="region" aria-label="SOTA intelligence modules">
        <hu-section-header
          heading="Intelligence Modules"
          description="State-of-the-art reasoning features"
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner">
            <div class="intel-badges">
              ${modules.map(
                (m) => html`
                  <span
                    class="intel-badge ${intel[m.key] ? "enabled" : "disabled"}"
                    title="${intel[m.key] ? "Enabled" : "Disabled"}"
                  >
                    ${intel[m.key] ? icons.check : icons.xCircle} ${m.label}
                  </span>
                `,
              )}
            </div>
          </div>
        </hu-card>
      </div>
    `;
  }

  private _goToHulaView(): void {
    window.location.hash = "hula";
  }

  private _renderHulaObservability() {
    const turns =
      this.snapshot.bth && typeof this.snapshot.bth.hula_tool_turns === "number"
        ? this.snapshot.bth.hula_tool_turns
        : 0;
    return html`
      <div class="section hu-scroll-reveal" role="region" aria-label="HuLa orchestration metrics">
        <hu-section-header
          heading="HuLa orchestration"
          description="Multi-step HuLa programs and persisted execution traces (same directory as HU_HULA_TRACE_DIR or ~/.human/hula_traces on POSIX)."
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner hula-cta">
            <div class="metric-grid">
              <div class="metric-item">
                <span class="metric-label">HuLa tool turns (BTH)</span>
                <span class="metric-value">${turns.toLocaleString()}</span>
              </div>
            </div>
            <hu-button
              variant="secondary"
              data-testid="metrics-open-hula-traces"
              @click=${this._goToHulaView}
              aria-label="Open HuLa traces view"
              >Open HuLa traces</hu-button
            >
          </div>
        </hu-card>
      </div>
    `;
  }

  private _renderEvalCalibration() {
    const bth = this.snapshot.bth;
    if (!bth) return nothing;
    const ab = typeof bth.ab_evaluations === "number" ? bth.ab_evaluations : 0;
    const alt = typeof bth.ab_alternates_chosen === "number" ? bth.ab_alternates_chosen : 0;
    const altRate = ab > 0 ? ((alt / ab) * 100).toFixed(1) : "—";
    const altRateStr = altRate === "—" ? "—" : `${altRate}%`;

    return html`
      <div
        class="section hu-cv-defer hu-scroll-reveal"
        role="region"
        aria-label="Evaluation metrics"
      >
        <hu-section-header
          heading="Evaluation &amp; A/B"
          description="Live BTH counters from the runtime. For SQLite pass-rate history, run human eval trend on the gateway host."
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner">
            <div class="metric-grid">
              <div class="metric-item">
                <span class="metric-label">A/B evaluations</span>
                <span class="metric-value">${ab.toLocaleString()}</span>
              </div>
              <div class="metric-item">
                <span class="metric-label">Alternates chosen</span>
                <span class="metric-value">${alt.toLocaleString()}</span>
              </div>
              <div class="metric-item">
                <span class="metric-label">Alternate rate</span>
                <span class="metric-value">${altRateStr}</span>
              </div>
            </div>
          </div>
        </hu-card>
      </div>
    `;
  }

  private _renderIntelligencePipeline() {
    const bth = this.snapshot.bth;
    if (!bth) return nothing;

    return html`
      <div
        class="section hu-cv-defer hu-scroll-reveal"
        role="region"
        aria-label="Intelligence pipeline metrics"
      >
        <hu-section-header
          heading="Intelligence Pipeline"
          description="BTH pipeline metrics by category"
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner">
            ${Object.entries(BTH_GROUPS).map(
              ([groupName, keys]) => html`
                <div class="bth-group">
                  <div class="metric-label" style="margin-bottom: var(--hu-space-sm)">
                    ${groupName}
                  </div>
                  <div class="metric-grid">
                    ${keys.map((key) => {
                      const val = bth[key];
                      return html`
                        <div class="metric-item">
                          <span class="metric-label">${labelFromKey(key)}</span>
                          <span class="metric-value"
                            >${val != null ? (val as number).toLocaleString() : "—"}</span
                          >
                        </div>
                      `;
                    })}
                  </div>
                </div>
              `,
            )}
          </div>
        </hu-card>
      </div>
    `;
  }

  override render() {
    const h = this.snapshot.health;
    const m = this.snapshot.metrics;
    const bth = this.snapshot.bth;
    const uptime = h?.uptime_seconds ?? 0;
    const activeSessions = m?.active_sessions ?? 0;
    const avgLatency = m?.avg_latency_ms ?? 0;
    const totalRequests = m?.total_requests ?? 0;
    const totalErrors = m?.total_errors ?? 0;
    const errorRate = totalRequests > 0 ? ((totalErrors / totalRequests) * 100).toFixed(2) : "0";
    const totalTurns = bth?.total_turns ?? m?.total_requests ?? 0;

    return html`
      <hu-page-hero role="region" aria-label="Observability">
        <hu-section-header
          heading="Observability"
          description="Live system health and intelligence metrics"
        >
        </hu-section-header>
      </hu-page-hero>

      <hu-stats-row class="hu-scroll-reveal-stagger">
        <hu-stat-card
          .valueStr=${formatUptime(uptime)}
          label="Uptime"
          style="--hu-stagger-delay: 0ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${activeSessions}
          label="Active Sessions"
          style="--hu-stagger-delay: 50ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${avgLatency}
          label="Avg Latency (ms)"
          style="--hu-stagger-delay: 100ms"
        ></hu-stat-card>
        <hu-stat-card
          .valueStr="${errorRate}%"
          label="Error Rate"
          style="--hu-stagger-delay: 150ms"
        ></hu-stat-card>
        <hu-stat-card
          .value=${totalTurns}
          label="Total Turns"
          style="--hu-stagger-delay: 200ms"
        ></hu-stat-card>
      </hu-stats-row>

      ${this.error
        ? html`<hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
            <hu-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry loading metrics"
              >Retry</hu-button
            >
          </hu-empty-state>`
        : nothing}
      ${this.loading
        ? this._renderSkeleton()
        : html`
            ${this._renderFidelity()} ${this._renderDirectiveTelemetry()}
            ${this._renderIntelligenceStats()}
            ${this._renderEvalCalibration()} ${this._renderHulaObservability()}
            ${this._renderSystemHealth()} ${this._renderIntelligencePipeline()}
          `}
    `;
  }

  private _renderFidelity() {
    /* The fidelity tile is its own component — it owns its loading
     * skeleton and error banner, so we just hand it the data and
     * an optional `errorMessage`. When `metrics.fidelity` returned
     * a payload but the field surface is empty (e.g. the agent
     * has no persona configured), the C handler emits an
     * `error: "no persona configured"` field which we surface here. */
    const errMsg =
      this.fidelityError ||
      (this.fidelity && (this.fidelity as FidelityStatus & { error?: string }).error) ||
      "";
    return html`
      <div class="section hu-scroll-reveal" role="region" aria-label="Persona fidelity">
        <hu-section-header
          heading="Persona Fidelity"
          description="LoRA-adapter delta against the active persona's example bank"
        ></hu-section-header>
        <hu-fidelity-tile .data=${this.fidelity} .errorMessage=${errMsg}></hu-fidelity-tile>
      </div>
    `;
  }

  private _renderDirectiveTelemetry() {
    return html`
      <div
        class="section hu-scroll-reveal"
        role="region"
        aria-label="Directive variant telemetry"
      >
        <hu-section-header
          heading="Directive Variant Telemetry"
          description="Distribution of acknowledgment-directive variants fired across all channels"
        ></hu-section-header>
        <hu-directive-telemetry-tile
          .data=${this.directiveTelemetry}
          .errorMessage=${this.directiveTelemetryError}
        ></hu-directive-telemetry-tile>
      </div>
    `;
  }
}
