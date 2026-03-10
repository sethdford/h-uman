import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { formatRelative } from "../utils.js";
import type { ActivityEvent } from "./hu-activity-feed.js";
import type { TimelineItem } from "./hu-timeline.js";
import "../components/hu-card.js";
import "../components/hu-chart.js";
import "../components/hu-timeline.js";

@customElement("hu-activity-timeline")
export class ScActivityTimeline extends LitElement {
  @property({ type: Array }) events: ActivityEvent[] = [];
  @property({ type: Number }) maxItems = 10;

  static override styles = css`
    :host {
      display: block;
    }

    .section-label {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-semibold, 600);
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--hu-accent-text, var(--hu-accent));
      margin-bottom: var(--hu-space-sm);
    }

    .activity-sparkline {
      margin-bottom: var(--hu-space-md);
    }
  `;

  private get _sparklineData() {
    const now = Date.now();
    const hourMs = 60 * 60 * 1000;
    const buckets: number[] = [];
    const labels: string[] = [];
    for (let i = 11; i >= 0; i--) {
      const bucketStart = now - (i + 1) * hourMs;
      const bucketEnd = now - i * hourMs;
      const count = this.events.filter((ev) => {
        const t = typeof ev.time === "number" ? ev.time : Date.now();
        return t >= bucketStart && t < bucketEnd;
      }).length;
      buckets.push(count);
      labels.push(i === 0 ? "Now" : `${i}h`);
    }
    return {
      labels,
      datasets: [{ data: buckets, color: "var(--hu-chart-brand)" }],
    };
  }

  private get _timelineItems(): TimelineItem[] {
    type Ev = ActivityEvent & {
      message?: string;
      text?: string;
      level?: string;
      detail?: string;
    };
    return this.events.map((ev: Ev) => {
      let message = ev.message ?? ev.text ?? "";
      if (!message && ev.type === "message") {
        message = `${ev.user ?? ""} via ${ev.channel ?? ""}: ${ev.preview ?? ""}`.trim();
      } else if (!message && ev.type === "tool_exec") {
        message = `Tool ${ev.tool ?? ""}: ${ev.command ?? ""}`.trim();
      } else if (!message && ev.type === "session_start") {
        message = `Session ${ev.session ?? ""} started`.trim();
      }
      if (!message) message = "Activity";
      const ts = typeof ev.time === "number" ? ev.time : Date.now();
      return {
        time: formatRelative(ts),
        message,
        status: (ev.level === "error" ? "error" : ev.level === "success" ? "success" : "info") as
          | "success"
          | "error"
          | "info"
          | "pending",
        detail: ev.detail,
      };
    });
  }

  override render() {
    return html`
      <div class="section-label">Live Activity</div>
      ${this.events.length > 0
        ? html`
            <div class="activity-sparkline">
              <hu-chart type="line" .data=${this._sparklineData} height=${48}></hu-chart>
            </div>
          `
        : nothing}
      <hu-timeline .items=${this._timelineItems} .max=${this.maxItems}></hu-timeline>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-activity-timeline": ScActivityTimeline;
  }
}
