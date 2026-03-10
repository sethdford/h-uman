import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

export interface TimelineItem {
  time: string;
  message: string;
  status: "success" | "error" | "info" | "pending";
  detail?: string;
}

@customElement("hu-timeline")
export class ScTimeline extends LitElement {
  @property({ type: Array }) items: TimelineItem[] = [];
  @property({ type: Number }) max = 10;

  static override styles = css`
    :host {
      display: block;
    }
    .timeline {
      display: flex;
      flex-direction: column;
      position: relative;
    }
    .entry {
      display: grid;
      grid-template-columns: 12px 1fr;
      gap: var(--hu-space-md);
      padding-bottom: var(--hu-space-md);
      position: relative;
      animation: hu-slide-up var(--hu-duration-normal) var(--hu-ease-out) both;
    }
    .dot {
      width: 8px;
      height: 8px;
      border-radius: var(--hu-radius-full);
      margin-top: var(--hu-space-xs);
      flex-shrink: 0;
      justify-self: center;
    }
    .dot.success {
      background: var(--hu-success);
    }
    .dot.error {
      background: var(--hu-error);
    }
    .dot.info {
      background: var(--hu-accent);
    }
    .dot.pending {
      background: var(--hu-text-muted);
    }
    .line {
      position: absolute;
      left: var(--hu-space-xs);
      top: var(--hu-icon-sm);
      bottom: 0;
      width: 1px;
      background: var(--hu-border-subtle);
    }
    .entry:last-child .line {
      display: none;
    }
    .time {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-faint);
      font-variant-numeric: tabular-nums;
    }
    .message {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }
    .detail {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }
    .content {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }
    .empty {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      padding: var(--hu-space-lg) 0;
    }
    @media (prefers-reduced-motion: reduce) {
      .entry {
        animation: none;
      }
    }
  `;

  override render() {
    const visible = this.items.slice(0, this.max);
    return html`
      <div class="timeline">
        ${visible.map(
          (item, i) => html`
            <div class="entry" style="animation-delay: ${i * 50}ms">
              <div class="dot ${item.status}"></div>
              <div class="line"></div>
              <div class="content">
                <span class="time">${item.time}</span>
                <span class="message">${item.message}</span>
                ${item.detail ? html`<span class="detail">${item.detail}</span>` : nothing}
              </div>
            </div>
          `,
        )}
      </div>
      ${this.items.length === 0 ? html`<div class="empty">No recent activity</div>` : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-timeline": ScTimeline;
  }
}
