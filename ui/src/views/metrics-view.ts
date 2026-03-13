import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-card.js";
import "../components/hu-skeleton.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";
import { icons } from "../icons.js";

interface MetricsSnapshot {
  health?: { uptime_seconds?: number; pid?: number };
  intelligence?: {
    tree_of_thought?: boolean;
    constitutional_ai?: boolean;
    llm_compiler?: boolean;
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

function friendlyError(e: unknown): string {
  const msg = e instanceof Error ? e.message : String(e);
  if (msg.includes("timeout")) return "Request timed out. Please try again.";
  if (msg.includes("WebSocket")) return "Connection lost. Reconnecting...";
  if (msg.includes("404")) return "Resource not found.";
  if (msg.includes("401") || msg.includes("unauthorized"))
    return "Authentication failed. Please check your credentials.";
  if (msg.includes("403") || msg.includes("forbidden")) return "Access denied.";
  if (msg.includes("network")) return "Network error. Please check your connection.";
  return "Something went wrong. Please try again.";
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
};

function labelFromKey(key: string): string {
  return key.replace(/_/g, " ").replace(/\b\w/g, (c) => c.toUpperCase());
}

@customElement("hu-metrics-view")
export class ScMetricsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 10_000;

  static override styles = css`
    :host {
      view-transition-name: view-metrics;
      display: block;
      color: var(--hu-text);
      max-width: 60rem;
    }
    .section {
      margin-bottom: var(--hu-space-2xl);
    }
    .metric-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(10rem, 1fr));
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
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
        transition-duration: 0s !important;
      }
    }
  `;

  @state() private snapshot: MetricsSnapshot = {};
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
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
      <div class="section" role="region" aria-label="System health metrics">
        <hu-section-header
          heading="System Health"
          description="Gateway and request metrics"
        ></hu-section-header>
        <hu-card glass>
          <div class="card-inner">
            <div class="metric-grid">
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
      { key: "speculative_cache", label: "Speculative Cache" },
    ];

    return html`
      <div class="section" role="region" aria-label="SOTA intelligence modules">
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

  private _renderIntelligencePipeline() {
    const bth = this.snapshot.bth;
    if (!bth) return nothing;

    return html`
      <div class="section" role="region" aria-label="Intelligence pipeline metrics">
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

      <hu-stats-row>
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
            ${this._renderIntelligenceStats()} ${this._renderSystemHealth()}
            ${this._renderIntelligencePipeline()}
          `}
    `;
  }
}
