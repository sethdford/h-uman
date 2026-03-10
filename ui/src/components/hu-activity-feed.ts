import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

export interface ActivityEvent {
  type: string;
  channel?: string;
  user?: string;
  preview?: string;
  tool?: string;
  command?: string;
  session?: string;
  time: number;
}

@customElement("hu-activity-feed")
export class ScActivityFeed extends LitElement {
  @property({ type: Array }) events: ActivityEvent[] = [];
  @property({ type: Number }) max = 6;

  static override styles = css`
    :host {
      display: block;
    }
    .feed {
      display: flex;
      flex-direction: column;
      gap: 0;
    }
    .event {
      display: flex;
      align-items: flex-start;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) 0;
      border-bottom: 1px solid color-mix(in srgb, var(--hu-border) 50%, transparent);
      animation: hu-feed-in var(--hu-duration-normal) var(--hu-spring-out) both;
    }
    .event:last-child {
      border-bottom: none;
    }
    @keyframes hu-feed-in {
      from {
        opacity: 0;
        transform: translateX(-8px);
      }
      to {
        opacity: 1;
        transform: translateX(0);
      }
    }
    .event-icon {
      width: var(--hu-icon-md);
      height: var(--hu-icon-md);
      flex-shrink: 0;
      margin-top: var(--hu-space-2xs);
    }
    .event-icon svg {
      width: 100%;
      height: 100%;
    }
    .event-icon.message {
      color: var(--hu-accent);
    }
    .event-icon.tool {
      color: var(--hu-warning);
    }
    .event-icon.session {
      color: var(--hu-success);
    }
    .event-body {
      flex: 1;
      min-width: 0;
    }
    .event-summary {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      line-height: var(--hu-leading-normal);
    }
    .event-summary strong {
      font-weight: var(--hu-weight-semibold);
    }
    .event-time {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-faint);
      font-variant-numeric: tabular-nums;
      white-space: nowrap;
      margin-top: var(--hu-space-2xs);
    }
    .empty {
      text-align: center;
      padding: var(--hu-space-lg);
      color: var(--hu-text-muted);
      font-size: var(--hu-text-sm);
    }

    @media (prefers-reduced-motion: reduce) {
      .event {
        animation: none;
      }
    }
  `;

  private _relativeTime(ts: number): string {
    const diff = Date.now() - ts;
    if (diff < 60000) return "just now";
    if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
    if (diff < 86400000) return `${Math.floor(diff / 3600000)}h ago`;
    return `${Math.floor(diff / 86400000)}d ago`;
  }

  private _renderEvent(ev: ActivityEvent) {
    let icon = icons["chat-circle"];
    let iconClass = "message";
    let summary = html`Activity`;

    if (ev.type === "message") {
      icon = icons["chat-circle"];
      iconClass = "message";
      summary = html`<strong>${ev.user}</strong> via ${ev.channel}: ${ev.preview}`;
    } else if (ev.type === "tool_exec") {
      icon = icons.terminal;
      iconClass = "tool";
      summary = html`Tool <strong>${ev.tool}</strong>: ${ev.command}`;
    } else if (ev.type === "session_start") {
      icon = icons["message-square"];
      iconClass = "session";
      summary = html`Session <strong>${ev.session}</strong> started`;
    }

    return html`
      <div class="event" role="article">
        <div class="event-icon ${iconClass}">${icon}</div>
        <div class="event-body">
          <div class="event-summary">${summary}</div>
        </div>
        <div class="event-time">${this._relativeTime(ev.time)}</div>
      </div>
    `;
  }

  override render() {
    const visible = this.events.slice(0, this.max);
    if (visible.length === 0) {
      return html`<div class="empty">No recent activity</div>`;
    }
    return html`
      <div class="feed" role="log" aria-live="polite">
        ${visible.map((ev) => this._renderEvent(ev))}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-activity-feed": ScActivityFeed;
  }
}
