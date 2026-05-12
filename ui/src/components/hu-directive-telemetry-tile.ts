/**
 * Directive telemetry dashboard tile.
 *
 * Surfaces the JSON output of the gateway's `metrics.directive_telemetry`
 * RPC method — `{total: N, variants: {null_overlay, default,
 * formal_terse, casual_emoji, casual_or_short, adaptive_emoji}}` — as
 * a stacked-bar visualization showing the distribution of which
 * acknowledgment-directive variant fired across all channels.
 *
 * Operators use this to spot whether channel overlays are doing
 * useful work: a tile dominated by `null_overlay` / `default` means
 * channels haven't been populated; a healthy distribution across
 * `formal_terse` / `casual_emoji` / `casual_or_short` / `adaptive_emoji`
 * means the per-channel overlays are routing correctly.
 *
 * The data contract intentionally matches the C gateway handler
 * (`cp_admin_metrics_directive_telemetry` in `src/gateway/cp_admin.c`)
 * verbatim — no wrapping, no renaming. The variant order in
 * `DIRECTIVE_VARIANTS` must stay in lockstep with
 * `hu_personal_model_directive_variant_label()` in
 * `src/memory/personal_model.c`; we iterate that const, never
 * `Object.entries(data.variants)`, so adding a variant on the C side
 * without touching this file is a build/test failure rather than a
 * silent under-report.
 */

import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-card.js";

export interface DirectiveTelemetry {
  total: number;
  variants: Record<string, number>;
}

/* Stable order — matches `hu_personal_model_directive_variant_label`
 * in src/memory/personal_model.c. The component renders segments in
 * this exact sequence regardless of how the gateway JSON is keyed. */
export const DIRECTIVE_VARIANTS = [
  "null_overlay",
  "default",
  "formal_terse",
  "casual_emoji",
  "casual_or_short",
  "adaptive_emoji",
] as const;

type VariantKey = (typeof DIRECTIVE_VARIANTS)[number];

/* Color mapping per design A.md §3:
 *   null_overlay  → --hu-warning      (signal: missing overlay)
 *   default       → --hu-text-muted   (signal: no useful steering)
 *   formal_terse  → categorical-2 (steel blue)
 *   casual_emoji  → categorical-1 (Human green; expected dominant)
 *   casual_or_short → categorical-5 (teal)
 *   adaptive_emoji → categorical-6 (light steel)
 *
 * categorical-3 (amber) and categorical-4 (coral) deliberately skipped
 * because they collide with --hu-warning / --hu-error semantics. */
const VARIANT_CLASS: Record<VariantKey, string> = {
  null_overlay: "warn",
  default: "muted",
  formal_terse: "cat2",
  casual_emoji: "cat1",
  casual_or_short: "cat5",
  adaptive_emoji: "cat6",
};

const safeCount = (data: DirectiveTelemetry, key: string): number => {
  const v = data.variants?.[key];
  return typeof v === "number" && v >= 0 ? v : 0;
};

const formatPercent = (count: number, total: number): string => {
  if (total <= 0) return "0%";
  const pct = (count / total) * 100;
  return `${pct.toFixed(1)}%`;
};

@customElement("hu-directive-telemetry-tile")
export class HuDirectiveTelemetryTile extends LitElement {
  /**
   * The JSON object emitted by `metrics.directive_telemetry`.
   * `null` means "not loaded yet" → renders the loading skeleton.
   */
  @property({ attribute: false }) data: DirectiveTelemetry | null = null;

  /**
   * When set, the tile renders an inline error banner instead of
   * the stacked bar. Mirrors the parent view's failure-isolation
   * pattern from `<hu-fidelity-tile>`.
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

    .bar {
      display: flex;
      gap: 2px;
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
      letter-spacing: 0.04em;
    }

    .segment {
      min-width: 2px;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
    }

    .segment--warn {
      background: var(--hu-warning, var(--hu-accent-secondary));
    }

    .segment--muted {
      background: var(--hu-text-muted);
    }

    .segment--cat1 {
      background: var(--hu-chart-categorical-1, var(--hu-accent));
    }

    .segment--cat2 {
      background: var(--hu-chart-categorical-2, var(--hu-accent-tertiary));
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
      border-radius: 2px;
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
          aria-label="Directive variant telemetry"
          aria-live="polite"
          aria-busy=${this.data == null && !this.errorMessage ? "true" : "false"}
        >
          ${this.renderHeader()}${this.renderBody()}
        </div>
      </hu-card>
    `;
  }

  private renderHeader() {
    const total = this.data?.total ?? 0;
    return html`
      <div class="header">
        <div class="title">Acknowledgment-directive variants</div>
        <div class="total-readout"><span class="total-value">${total}</span> total fires</div>
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

  private renderBar(data: DirectiveTelemetry) {
    const variantSum = DIRECTIVE_VARIANTS.reduce((acc, key) => acc + safeCount(data, key), 0);

    if (variantSum === 0) {
      return html`<div class="bar-empty" role="status" aria-label="No variants fired yet">
        no variants fired yet
      </div>`;
    }

    /* Filter zero-count variants before render: a `flex-grow: 0`
     * segment is 0px wide but still adds a `gap` slot, which would
     * show as phantom whitespace. The legend always lists all six
     * variants so no information is hidden. */
    const segments = DIRECTIVE_VARIANTS.map((key) => ({
      key,
      count: safeCount(data, key),
    })).filter((s) => s.count > 0);

    return html`
      <div class="bar" role="list" aria-label="Variant distribution">
        ${segments.map(
          (s) => html`
            <div
              class="segment segment--${VARIANT_CLASS[s.key]}"
              role="listitem"
              aria-label="${s.key}: ${s.count} fires (${formatPercent(s.count, variantSum)})"
              style="flex-grow: ${s.count}"
            ></div>
          `,
        )}
      </div>
    `;
  }

  private renderLegend(data: DirectiveTelemetry) {
    const variantSum = DIRECTIVE_VARIANTS.reduce((acc, key) => acc + safeCount(data, key), 0);
    return html`
      <div class="legend" aria-hidden="true">
        ${DIRECTIVE_VARIANTS.map((key) => {
          const count = safeCount(data, key);
          return html`
            <span class="legend-item">
              <span class="legend-swatch segment--${VARIANT_CLASS[key]}"></span>
              <span>${key}</span>
              <span class="legend-count">${count}</span>
              <span>(${formatPercent(count, variantSum)})</span>
            </span>
          `;
        })}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-directive-telemetry-tile": HuDirectiveTelemetryTile;
  }
}
