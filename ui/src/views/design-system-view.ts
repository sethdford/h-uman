import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { LitElement } from "lit";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-card.js";
import "../components/hu-switch.js";
import "../components/hu-chart.js";
import "../components/hu-ring-progress.js";
import "../components/hu-radial-gauge.js";
import "../components/hu-timeline-chart.js";
import "../components/hu-sankey.js";
import "../components/hu-activity-heatmap.js";
import "../components/hu-sparkline.js";
import "../components/hu-forecast-chart.js";
import "../components/hu-animated-value.js";
import "../components/hu-stat-card.js";
import "../components/hu-progress.js";
// Quiet Room assets load with this view: its switch is the only way to set
// data-brand="quiet", so keeping them here holds ~9 KB of CSS out of the
// entry bundle (ui/scripts/check-bundle-size.sh). Once loaded they are
// document-level, so the preview still applies to other views.
import "../styles/_quiet.css";
import "@fontsource-variable/newsreader/opsz.css";
import "@fontsource-variable/newsreader/opsz-italic.css";
import {
  DEMO_BAR_CHART,
  DEMO_DOUGHNUT_CHART,
  DEMO_FORECAST_HISTORY,
  DEMO_LINE_CHART,
  DEMO_RING_PROGRESS,
  DEMO_SANKEY_LINKS,
  DEMO_SANKEY_NODES,
  DEMO_SPARKLINE,
  DEMO_TIMELINE_BARS,
  DEMO_TIMELINE_TODAY,
  DESIGN_CATEGORICAL_SWATCHES,
  demoHeatmapData,
} from "../design-system/demo-data.js";

/**
 * In-app design system + data visualization gallery.
 * Fixtures live in `src/design-system/demo-data.ts`; styling uses `--hu-*` tokens only.
 */
@customElement("hu-design-system-view")
export class HuDesignSystemView extends LitElement {
  private readonly _heatmap = demoHeatmapData();

  /** Mirrors <html data-brand="quiet">; persists for the session so other views preview it too. */
  @state() private _quiet = document.documentElement.dataset.brand === "quiet";

  private _onQuietChange(e: CustomEvent<{ checked: boolean }>) {
    this._quiet = e.detail.checked;
    if (this._quiet) document.documentElement.dataset.brand = "quiet";
    else delete document.documentElement.dataset.brand;
  }

  static override styles = css`
    :host {
      view-transition-name: view-design-system;
      display: block;
      width: 100%;
      color: var(--hu-text);
      max-width: 60rem;
      min-width: 0;
      margin: 0 auto;
      padding-bottom: var(--hu-space-3xl);
      contain: layout style;
      container-type: inline-size;
    }

    .section {
      margin-bottom: var(--hu-space-2xl);
    }

    .meta {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      margin: 0 0 var(--hu-space-lg);
      line-height: 1.5;
      overflow-wrap: break-word;
    }

    .foundations-grid {
      display: grid;
      gap: var(--hu-space-adaptive-content-gap);
      grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-md), 100%), 1fr));
    }

    .viz-grid {
      display: grid;
      gap: var(--hu-space-adaptive-content-gap);
      grid-template-columns: repeat(auto-fill, minmax(min(var(--hu-grid-track-lg), 100%), 1fr));
    }

    .card-title {
      margin: 0 0 var(--hu-space-md);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .type-sample {
      margin: 0;
      font-size: var(--hu-text-2xl);
      font-weight: var(--hu-weight-bold);
      color: var(--hu-text);
    }

    .type-body {
      margin: var(--hu-space-sm) 0 0;
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .swatch-row {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
      align-items: center;
    }

    .swatch {
      width: 2.5rem;
      height: 2.5rem;
      border-radius: var(--hu-radius-md);
      border: 1px solid var(--hu-border-subtle);
    }

    .surface-row {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
    }

    .surface-tile {
      padding: var(--hu-space-md);
      border-radius: var(--hu-radius-md);
      border: 1px solid var(--hu-border-subtle);
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
    }

    .surface-tile.default {
      background: var(--hu-surface-container);
    }

    .surface-tile.high {
      background: var(--hu-surface-container-high);
    }

    .surface-tile.highest {
      background: var(--hu-surface-container-highest);
    }

    .glass-sample {
      padding: var(--hu-space-lg);
      border-radius: var(--hu-radius-lg);
    }

    .viz-center {
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 5rem;
    }

    .viz-wide {
      grid-column: 1 / -1;
    }

    @container (max-width: 40rem) {
      .viz-grid .viz-wide {
        grid-column: auto;
      }
    }

    .stat-row {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-md);
      align-items: stretch;
    }

    code.path {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-accent-text, var(--hu-accent));
      overflow-wrap: anywhere;
    }

    .quiet-toggle {
      margin-top: var(--hu-space-md);
    }

    /* Own size container so 14cqi tracks this card, not hu-card's shadow internals. */
    .quiet-card {
      container-type: inline-size;
    }

    .quiet-display {
      font-family: var(--hu-type-display-lg-family, var(--hu-font));
      /* Capped by card width: at 6rem, "Actually" overflowed the grid card. */
      font-size: min(var(--hu-text-display-xl, var(--hu-text-3xl)), 14cqi);
      font-weight: var(--hu-type-display-lg-weight);
      letter-spacing: var(--hu-type-display-lg-letter-spacing);
      line-height: 1;
      margin: 0 0 var(--hu-space-sm);
    }

    .quiet-display em {
      color: var(--hu-accent-text);
    }
  `;

  override render() {
    return html`
      <hu-page-hero role="region" aria-label="Design system">
        <hu-section-header
          heading="Design system"
          description="Tokens, surfaces, and data visualization — authored in TypeScript; rendered with dashboard components."
        ></hu-section-header>
        <p class="meta">
          Source of truth for names and values:
          <code class="path">design-tokens/*.tokens.json</code>
          → <code class="path">design-tokens/build.ts</code> →
          <code class="path">ui/src/styles/theme.css</code> and
          <code class="path">--hu-*</code> custom properties. Demo datasets:
          <code class="path">ui/src/design-system/demo-data.ts</code>.
        </p>
        <hu-switch
          class="quiet-toggle"
          data-testid="quiet-toggle"
          label="Quiet Room preview"
          .checked=${this._quiet}
          @hu-change=${this._onQuietChange}
        ></hu-switch>
      </hu-page-hero>

      <section class="section" aria-label="Foundations">
        <hu-section-header
          heading="Foundations"
          description="Typography, brand accents, categorical chart ramp, tonal surfaces, glass."
        ></hu-section-header>
        <div class="foundations-grid">
          <hu-card>
            <div class="quiet-card">
              <h3 class="card-title">Quiet Room display</h3>
              <p class="quiet-display">Actually <em>yours.</em></p>
              <p class="type-body">
                Newsreader with optical sizing when the preview is on; the UI face otherwise.
                <code class="path">--hu-font-display</code>,
                <code class="path">--hu-text-display-xl</code>.
              </p>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Typography</h3>
            <p class="type-sample">Avenir + tabular numerals</p>
            <p class="type-body">
              Weights and sizes come from design tokens (<code class="path">--hu-text-*</code>,
              <code class="path">--hu-weight-*</code>).
            </p>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Brand accents</h3>
            <div class="swatch-row" role="list">
              <span
                class="swatch"
                style="background:var(--hu-accent)"
                title="Primary"
                role="listitem"
              ></span>
              <span
                class="swatch"
                style="background:var(--hu-accent-secondary)"
                title="Secondary"
                role="listitem"
              ></span>
              <span
                class="swatch"
                style="background:var(--hu-accent-tertiary)"
                title="Tertiary"
                role="listitem"
              ></span>
            </div>
            <p class="type-body">60-30-10 hierarchy — see visual standards.</p>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Chart categorical ramp</h3>
            <div class="swatch-row" role="list">
              ${DESIGN_CATEGORICAL_SWATCHES.map(
                (c) => html`<span class="swatch" style="background:${c}" role="listitem"></span>`,
              )}
            </div>
            <p class="type-body">
              Aligned with <code class="path">data-viz.tokens.json</code> series count.
            </p>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Tonal surfaces (M3)</h3>
            <div class="surface-row">
              <div class="surface-tile default">container</div>
              <div class="surface-tile high">high</div>
              <div class="surface-tile highest">highest</div>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Glass</h3>
            <div class="glass-sample hu-glass-subtle">
              <span class="type-body"
                >Utility <code class="path">.hu-glass-subtle</code> on chart panels.</span
              >
            </div>
          </hu-card>
        </div>
      </section>

      <section class="section" aria-label="Data visualization">
        <hu-section-header
          heading="Data visualization"
          description="Chart.js wrapper, custom SVG metrics, and motion-safe components."
        ></hu-section-header>
        <div class="viz-grid">
          <hu-card>
            <h3 class="card-title">Bar (hu-chart)</h3>
            <hu-chart type="bar" .data=${DEMO_BAR_CHART} .height=${200}></hu-chart>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Line (hu-chart)</h3>
            <hu-chart type="line" .data=${DEMO_LINE_CHART} .height=${200}></hu-chart>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Doughnut (hu-chart)</h3>
            <hu-chart type="doughnut" .data=${DEMO_DOUGHNUT_CHART} .height=${220}></hu-chart>
          </hu-card>
          <div class="viz-wide">
            <hu-card>
              <h3 class="card-title">Forecast (hu-forecast-chart)</h3>
              <hu-forecast-chart
                .history=${DEMO_FORECAST_HISTORY}
                .projectedTotal=${52}
                .daysInMonth=${31}
              ></hu-forecast-chart>
            </hu-card>
          </div>
          <hu-card>
            <h3 class="card-title">Activity heatmap</h3>
            <hu-activity-heatmap .data=${this._heatmap} .weeks=${12}></hu-activity-heatmap>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Ring progress</h3>
            <div class="viz-center">
              <hu-ring-progress .rings=${DEMO_RING_PROGRESS} .size=${160}></hu-ring-progress>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Radial gauge</h3>
            <div class="viz-center">
              <hu-radial-gauge
                .value=${62}
                .max=${100}
                unit="%"
                label="SLO budget"
                glass
              ></hu-radial-gauge>
            </div>
          </hu-card>
          <div class="viz-wide">
            <hu-card>
              <h3 class="card-title">Timeline</h3>
              <hu-timeline-chart
                .bars=${DEMO_TIMELINE_BARS}
                .today=${DEMO_TIMELINE_TODAY}
              ></hu-timeline-chart>
            </hu-card>
          </div>
          <div class="viz-wide">
            <hu-card>
              <h3 class="card-title">Sankey / pipeline</h3>
              <hu-sankey .nodes=${DEMO_SANKEY_NODES} .links=${DEMO_SANKEY_LINKS}></hu-sankey>
            </hu-card>
          </div>
          <hu-card>
            <h3 class="card-title">Sparkline</h3>
            <div class="viz-center">
              <hu-sparkline .data=${DEMO_SPARKLINE} .width=${120} .height=${36}></hu-sparkline>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Animated value</h3>
            <div class="viz-center">
              <hu-animated-value .value=${12847} format="compact" prefix="$"></hu-animated-value>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Stat card</h3>
            <div class="stat-row">
              <hu-stat-card
                label="Throughput"
                .value=${2400}
                suffix="rpm"
                trend="12%"
                trendDirection="up"
                .sparklineData=${DEMO_SPARKLINE}
              ></hu-stat-card>
            </div>
          </hu-card>
          <hu-card>
            <h3 class="card-title">Linear progress</h3>
            <hu-progress .value=${72} label="Rollout"></hu-progress>
          </hu-card>
        </div>
      </section>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-design-system-view": HuDesignSystemView;
  }
}
