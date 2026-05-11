/**
 * Persona-fidelity dashboard tile.
 *
 * Surfaces the JSON output of `human ml fidelity-status` —
 * `{persona, fingerprint_source, baseline:{...}, ab:{...}}` — as
 * a single dashboard card with three lanes:
 *
 *   1. Baseline mean (the upper bound a frontier model can hit
 *      without an adapter; rendered as a percentage 0-100%).
 *   2. A/B delta (after_mean − before_mean), shown when the
 *      backend reports `ab.available === true`. Rendered with
 *      direction tinting: positive uses the success accent,
 *      negative uses the error accent. When `ab.available === false`,
 *      the lane shows a muted placeholder rather than disappearing —
 *      consistency over surprise.
 *   3. Sample counts (baseline.scored, ab.scored_before/after),
 *      collapsed into a metric-row for at-a-glance numerator
 *      transparency.
 *
 * The data contract intentionally matches the CLI's JSON shape
 * verbatim — no wrapping, no renaming. A future gateway method
 * (e.g. `metrics.fidelity`) can pipe the JSON straight through.
 */

import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-card.js";
import "./hu-metric-row.js";

export interface FidelityBaseline {
  scored: number;
  mean: number;
  min: number;
  max: number;
}

export interface FidelityAb {
  available: boolean;
  before_mean?: number;
  after_mean?: number;
  delta?: number;
  scored_before?: number;
  scored_after?: number;
}

export interface FidelityStatus {
  persona: string;
  fingerprint_source: "personal_model" | "synthetic";
  baseline: FidelityBaseline;
  ab: FidelityAb;
}

const formatPercent = (x: number | undefined): string => {
  if (x === undefined || Number.isNaN(x)) return "—";
  return `${(x * 100).toFixed(1)}%`;
};

const formatSignedPercent = (x: number | undefined): string => {
  if (x === undefined || Number.isNaN(x)) return "—";
  const pct = x * 100;
  const sign = pct >= 0 ? "+" : "";
  return `${sign}${pct.toFixed(1)}%`;
};

@customElement("hu-fidelity-tile")
export class HuFidelityTile extends LitElement {
  /**
   * The full JSON object emitted by `human ml fidelity-status`.
   * `null` represents "data not loaded yet" and renders the
   * loading skeleton; `undefined` is treated identically.
   */
  @property({ attribute: false }) data: FidelityStatus | null = null;

  /**
   * When set, the tile renders an error banner instead of the
   * data lanes. Used by the parent view to surface gateway
   * failures without requiring a separate banner component.
   */
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
      letter-spacing: 0.01em;
    }

    .source {
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }

    .lanes {
      display: flex;
      gap: var(--hu-space-lg);
      flex-wrap: wrap;
    }

    .lane {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
      min-width: 8rem;
    }

    .lane-label {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .lane-value {
      font-size: var(--hu-text-xl);
      font-weight: var(--hu-weight-semibold);
      font-variant-numeric: tabular-nums;
      color: var(--hu-text);

      &.success {
        color: var(--hu-success);
      }
      &.error {
        color: var(--hu-error);
      }
      &.muted {
        color: var(--hu-text-muted);
      }
    }

    .lane-sub {
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
    }

    .skeleton {
      height: 2.5rem;
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
          aria-label="Persona fidelity status"
          aria-live="polite"
          aria-busy=${this.data == null && !this.errorMessage ? "true" : "false"}
        >
          ${this.renderHeader()}${this.renderBody()}
        </div>
      </hu-card>
    `;
  }

  private renderHeader() {
    const persona = this.data?.persona ?? "—";
    const source = this.data?.fingerprint_source;
    const sourceLabel =
      source === "personal_model"
        ? "from learned style"
        : source === "synthetic"
          ? "synthetic baseline"
          : "";
    return html`
      <div class="header">
        <div class="title">Persona fidelity · ${persona}</div>
        ${sourceLabel
          ? html`<div class="source" aria-label="Fingerprint source: ${sourceLabel}">
              ${sourceLabel}
            </div>`
          : nothing}
      </div>
    `;
  }

  private renderBody() {
    if (this.errorMessage) {
      return html`<div class="error-banner" role="alert">${this.errorMessage}</div>`;
    }
    if (!this.data) {
      return html`
        <div class="lanes" aria-hidden="true">
          <div class="lane"><div class="skeleton" style="width: 6rem"></div></div>
          <div class="lane"><div class="skeleton" style="width: 6rem"></div></div>
          <div class="lane"><div class="skeleton" style="width: 8rem"></div></div>
        </div>
      `;
    }
    const { baseline, ab } = this.data;
    const deltaClass = ab.available
      ? (ab.delta ?? 0) > 0
        ? "success"
        : (ab.delta ?? 0) < 0
          ? "error"
          : "muted"
      : "muted";
    return html`
      <div class="lanes">
        <div class="lane">
          <span class="lane-label">Baseline mean</span>
          <span class="lane-value">${formatPercent(baseline.mean)}</span>
          <span class="lane-sub"
            >range ${formatPercent(baseline.min)} – ${formatPercent(baseline.max)}</span
          >
        </div>
        <div class="lane">
          <span class="lane-label">LoRA A/B delta</span>
          <span class="lane-value ${deltaClass}">
            ${ab.available ? formatSignedPercent(ab.delta) : "no run"}
          </span>
          <span class="lane-sub">
            ${ab.available
              ? html`${formatPercent(ab.before_mean)} → ${formatPercent(ab.after_mean)}`
              : "run lora-runner twice to populate"}
          </span>
        </div>
        <div class="lane">
          <span class="lane-label">Samples scored</span>
          <span class="lane-value">${baseline.scored}</span>
          <span class="lane-sub">
            ${ab.available
              ? html`A/B: ${ab.scored_before ?? 0} / ${ab.scored_after ?? 0}`
              : "baseline only"}
          </span>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-fidelity-tile": HuFidelityTile;
  }
}
