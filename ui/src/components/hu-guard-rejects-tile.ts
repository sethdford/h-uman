/**
 * Response-guard reject telemetry tile.
 *
 * Surfaces `metrics.guard_rejects` from the gateway
 * (`cp_admin_metrics_guard_rejects` in `src/gateway/cp_admin.c`) —
 * cumulative REJECT counts per detector class (G1–G8 family).
 *
 * Operators use this to tune thresholds: a spike in `length_anomaly`
 * after a deploy suggests G5/EWMA regression; dominance of
 * `semantic_leak` means template/CoT detectors are doing the work.
 */

import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-card.js";

/** Flat JSON from `metrics.guard_rejects` — field names match C exactly. */
export interface GuardRejectStats {
  semantic_leak: number;
  length_anomaly: number;
  director_echo: number;
  persona_pii_echo: number;
  persona_identity_echo: number;
}

/** Stable render order; must match keys emitted by `cp_admin_metrics_guard_rejects`. */
export const GUARD_REJECT_KEYS = [
  "semantic_leak",
  "length_anomaly",
  "director_echo",
  "persona_pii_echo",
  "persona_identity_echo",
] as const;

export type GuardRejectKey = (typeof GUARD_REJECT_KEYS)[number];

const KEY_LABEL: Record<GuardRejectKey, string> = {
  semantic_leak: "semantic / template",
  length_anomaly: "length anomaly",
  director_echo: "director echo",
  persona_pii_echo: "persona PII",
  persona_identity_echo: "persona identity",
};

const KEY_CLASS: Record<GuardRejectKey, string> = {
  semantic_leak: "cat4",
  length_anomaly: "warn",
  director_echo: "cat2",
  persona_pii_echo: "cat5",
  persona_identity_echo: "cat6",
};

const safeCount = (data: GuardRejectStats, key: GuardRejectKey): number => {
  const v = data[key];
  return typeof v === "number" && v >= 0 ? v : 0;
};

export const totalGuardRejects = (data: GuardRejectStats): number =>
  GUARD_REJECT_KEYS.reduce((acc, key) => acc + safeCount(data, key), 0);

const formatPercent = (count: number, total: number): string => {
  if (total <= 0) return "0%";
  return `${((count / total) * 100).toFixed(1)}%`;
};

@customElement("hu-guard-rejects-tile")
export class HuGuardRejectsTile extends LitElement {
  @property({ attribute: false }) data: GuardRejectStats | null = null;
  /** Increment since the previous metrics poll (10s cadence on Observability). */
  @property({ type: Number }) deltaSinceRefresh = 0;
  @property({ type: String }) errorMessage = "";

  static override styles = css`
    :host {
      display: block;
      animation: hu-scale-in var(--hu-duration-normal) var(--hu-spring-micro, ease-out) both;
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        animation: none;
      }
    }

    .tile {
      padding: var(--hu-space-md);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .header {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: var(--hu-space-sm);
    }

    .title {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .total-readout {
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }

    .total-value {
      color: var(--hu-text);
      font-variant-numeric: tabular-nums;
    }

    .delta-readout {
      font-size: var(--hu-text-2xs);
      color: var(--hu-accent);
      font-variant-numeric: tabular-nums;
    }

    .bar {
      display: flex;
      gap: var(--hu-space-2xs);
      height: 1.25rem;
      width: 100%;
      border-radius: var(--hu-radius-sm);
      overflow: hidden;
      background: var(--hu-surface-container);
    }

    .bar-empty {
      display: flex;
      align-items: center;
      justify-content: center;
      height: 1.25rem;
      width: 100%;
      border-radius: var(--hu-radius-sm);
      background: var(--hu-surface-container);
      color: var(--hu-text-muted);
      font-size: var(--hu-text-2xs);
    }

    .segment {
      min-width: 2px;
    }

    .segment--warn {
      background: var(--hu-warning, var(--hu-accent-secondary));
    }

    .segment--cat2 {
      background: var(--hu-chart-categorical-2, var(--hu-accent-tertiary));
    }

    .segment--cat4 {
      background: var(--hu-chart-categorical-4, var(--hu-error));
    }

    .segment--cat5 {
      background: var(--hu-chart-categorical-5, var(--hu-accent));
    }

    .segment--cat6 {
      background: var(--hu-chart-categorical-6, var(--hu-text-muted));
    }

    .legend {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-sm) var(--hu-space-md);
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
    }

    .legend-item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
    }

    .legend-swatch {
      display: inline-block;
      width: 0.5rem;
      height: 0.5rem;
      border-radius: var(--hu-radius-sm);
    }

    .legend-count {
      color: var(--hu-text);
      font-variant-numeric: tabular-nums;
    }

    .skeleton {
      height: 1.25rem;
      width: 100%;
      background: var(--hu-surface-container);
      border-radius: var(--hu-radius-sm);
      animation: hu-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite alternate;
    }

    .error-banner {
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: color-mix(in srgb, var(--hu-error) 8%, transparent);
      border: 1px solid color-mix(in srgb, var(--hu-error) 24%, transparent);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-error-text, var(--hu-error));
      font-size: var(--hu-text-sm);
    }

    @keyframes hu-pulse {
      from {
        opacity: 0.45;
      }
      to {
        opacity: 0.85;
      }
    }
  `;

  override render() {
    return html`
      <hu-card glass surface="high">
        <div
          class="tile"
          role="region"
          aria-label="Response guard reject telemetry"
          aria-live="polite"
          aria-busy=${this.data == null && !this.errorMessage ? "true" : "false"}
        >
          ${this.renderHeader()}${this.renderBody()}
        </div>
      </hu-card>
    `;
  }

  private renderHeader() {
    const total = this.data ? totalGuardRejects(this.data) : 0;
    const delta =
      this.deltaSinceRefresh > 0
        ? html`<span class="delta-readout"> +${this.deltaSinceRefresh} since last refresh</span>`
        : nothing;
    return html`
      <div class="header">
        <div class="title">Response guard rejects</div>
        <div class="total-readout">
          <span class="total-value">${total}</span> total rejects${delta}
        </div>
      </div>
    `;
  }

  private renderBody() {
    if (this.errorMessage) {
      return html`<div class="error-banner" role="alert">${this.errorMessage}</div>`;
    }
    if (!this.data) {
      return html`<div class="skeleton" aria-hidden="true"></div>`;
    }
    return html`${this.renderBar(this.data)}${this.renderLegend(this.data)}`;
  }

  private renderBar(data: GuardRejectStats) {
    const total = totalGuardRejects(data);
    if (total === 0) {
      return html`<div class="bar-empty" role="status" aria-label="No guard rejects yet">
        no rejects recorded yet
      </div>`;
    }

    const segments = GUARD_REJECT_KEYS.map((key) => ({
      key,
      count: safeCount(data, key),
    })).filter((s) => s.count > 0);

    return html`
      <div class="bar" role="list" aria-label="Reject class distribution">
        ${segments.map(
          (s) => html`
            <div
              class="segment segment--${KEY_CLASS[s.key]}"
              role="listitem"
              aria-label="${KEY_LABEL[s.key]}: ${s.count} rejects (${formatPercent(
                s.count,
                total,
              )})"
              style="flex-grow: ${s.count}"
            ></div>
          `,
        )}
      </div>
    `;
  }

  private renderLegend(data: GuardRejectStats) {
    const total = totalGuardRejects(data);
    return html`
      <div class="legend" aria-hidden="true">
        ${GUARD_REJECT_KEYS.map((key) => {
          const count = safeCount(data, key);
          return html`
            <span class="legend-item">
              <span class="legend-swatch segment--${KEY_CLASS[key]}"></span>
              <span>${KEY_LABEL[key]}</span>
              <span class="legend-count">${count}</span>
              <span>(${formatPercent(count, total)})</span>
            </span>
          `;
        })}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-guard-rejects-tile": HuGuardRejectsTile;
  }
}
