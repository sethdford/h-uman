import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayClient } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { EVENT_NAMES } from "../utils.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-input.js";
import "../components/sc-button.js";
import "../components/sc-empty-state.js";
import "../components/sc-card.js";
import "../components/sc-segmented-control.js";
import "../components/sc-badge.js";
import "../components/sc-timeline.js";
import "../components/sc-skeleton.js";
import type { TimelineItem } from "../components/sc-timeline.js";

interface LogEntry {
  ts: string;
  event: string;
  payload: Record<string, unknown>;
}

const LEVEL_OPTIONS = [
  { value: "all", label: "All" },
  { value: EVENT_NAMES.CHAT, label: "Chat" },
  { value: EVENT_NAMES.TOOL_CALL, label: "Tool" },
  { value: EVENT_NAMES.ERROR, label: "Error" },
  { value: EVENT_NAMES.HEALTH, label: "Health" },
];

function formatRelativeTime(ts: string): string {
  try {
    const diff = Date.now() - new Date(ts).getTime();
    const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });
    if (diff < 0) return "now";
    if (diff < 60_000) return rtf.format(-Math.floor(diff / 1000), "second");
    if (diff < 3_600_000) return rtf.format(-Math.floor(diff / 60_000), "minute");
    if (diff < 86_400_000) return rtf.format(-Math.floor(diff / 3_600_000), "hour");
    return rtf.format(-Math.floor(diff / 86_400_000), "day");
  } catch {
    return ts;
  }
}

@customElement("sc-logs-view")
export class ScLogsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      view-transition-name: view-logs;
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
      color: var(--sc-text);
      max-width: 60rem;
    }
    .layout {
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-md);
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
    }
    .controls-sticky {
      position: sticky;
      top: 0;
      z-index: 1;
      background: var(--sc-bg);
      padding: var(--sc-space-md) 0;
      margin-bottom: var(--sc-space-sm);
      border-bottom: 1px solid var(--sc-border-subtle);
    }
    .controls {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
      flex-wrap: wrap;
    }
    .filter-input {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font-mono);
      width: 13.75rem;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .filter-input:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 var(--sc-space-xs) var(--sc-accent-subtle);
    }
    .filter-input::placeholder {
      color: var(--sc-text-muted);
    }
    .event.chat {
      color: var(--sc-success);
    }
    .event.tool-call {
      color: var(--sc-info);
    }
    .event.error {
      color: var(--sc-error);
    }
    .event.health {
      color: var(--sc-warning);
    }
    .log-card {
      flex: 1;
      display: flex;
      flex-direction: column;
      min-height: 0;
    }
    .log-area-wrapper {
      flex: 1;
      display: flex;
      flex-direction: column;
      min-height: 0;
    }
    .log-area {
      flex: 1;
      min-height: 0;
      padding: var(--sc-space-md);
      overflow-y: auto;
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      line-height: 1.6;
      color: var(--sc-text);
    }
    .log-area::-webkit-scrollbar {
      width: var(--sc-space-sm);
    }
    .log-area::-webkit-scrollbar-track {
      background: var(--sc-bg-elevated);
      border-radius: var(--sc-radius-sm);
    }
    .log-area::-webkit-scrollbar-thumb {
      background: var(--sc-border);
      border-radius: var(--sc-radius-sm);
    }
    .log-area::-webkit-scrollbar-thumb:hover {
      background: var(--sc-text-muted);
    }
    @media (max-width: 48rem) /* --sc-breakpoint-lg */ {
      .header {
        flex-wrap: wrap;
      }
      .controls {
        flex-wrap: wrap;
      }
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .filter-input {
        width: 100%;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .filter-input,
      .log-area {
        transition: none !important;
      }
    }
  `;

  @state() private logs: LogEntry[] = [];
  @state() private _buffer: LogEntry[] = [];
  @state() private filter = "";
  @state() private _level: string = "all";
  @state() private _paused = false;
  @state() private _relativeTimeKey = 0;
  @state() private loading = true;
  @state() private error = "";

  private _tsInterval: ReturnType<typeof setInterval> | null = null;
  private _logsStatusHandler = ((e: CustomEvent<string>) => {
    const status = e.detail;
    if (status === "connected") {
      this.loading = false;
      this.error = "";
    } else if (status === "disconnected") {
      this.loading = false;
      this.error = "Not connected";
    } else if (status === "connecting") {
      this.loading = true;
      this.error = "";
    }
  }) as EventListener;

  protected override async load(): Promise<void> {
    this.setupListener();
    if (this.gateway?.status === "connected") {
      this.loading = false;
      this.error = "";
    }
  }

  override connectedCallback(): void {
    super.connectedCallback();
    const gw = this.gateway;
    if (gw) {
      gw.addEventListener(GatewayClient.EVENT_STATUS, this._logsStatusHandler);
      const status = gw.status;
      if (status === "connected") {
        this.loading = false;
        this.error = "";
      } else if (status === "disconnected") {
        this.loading = false;
        this.error = "Not connected";
      } else {
        this.loading = true;
      }
    }
    this._tsInterval = setInterval(() => {
      this._relativeTimeKey++;
      this.requestUpdate();
    }, 10_000);
  }

  override disconnectedCallback(): void {
    this.gateway?.removeEventListener(GatewayClient.EVENT_STATUS, this._logsStatusHandler);
    if (this._tsInterval) {
      clearInterval(this._tsInterval);
      this._tsInterval = null;
    }
    this.removeListener();
    super.disconnectedCallback();
  }

  private setupListener(): void {
    const gw = this.gateway;
    if (!gw) return;
    gw.addEventListener(GatewayClient.EVENT_GATEWAY, this.handleGatewayEvent as EventListener);
  }

  private removeListener(): void {
    const gw = this.gateway;
    if (!gw) return;
    gw.removeEventListener(GatewayClient.EVENT_GATEWAY, this.handleGatewayEvent as EventListener);
  }

  private handleGatewayEvent = (e: Event): void => {
    const ev = (e as CustomEvent<{ event: string; payload: Record<string, unknown> }>).detail;
    const entry: LogEntry = {
      ts: new Date().toISOString(),
      event: ev.event,
      payload: ev.payload ?? {},
    };
    if (this._paused) {
      this._buffer = [...this._buffer, entry];
    } else {
      this.logs = [...this.logs, entry];
      this.scrollToBottom();
    }
    this.requestUpdate();
  };

  private togglePause(): void {
    this._paused = !this._paused;
    if (!this._paused && this._buffer.length > 0) {
      this.logs = [...this.logs, ...this._buffer];
      this._buffer = [];
      this.scrollToBottom();
    }
    this.requestUpdate();
  }

  /** Map log event to sc-timeline status */
  private timelineStatus(event: string): TimelineItem["status"] {
    switch (event) {
      case EVENT_NAMES.CHAT:
        return "success";
      case EVENT_NAMES.TOOL_CALL:
        return "info";
      case EVENT_NAMES.ERROR:
        return "error";
      case EVENT_NAMES.HEALTH:
        return "pending";
      default:
        return "pending";
    }
  }

  private toTimelineItems(entries: LogEntry[]): TimelineItem[] {
    return entries.map((l) => {
      const payloadStr = Object.keys(l.payload).length > 0 ? JSON.stringify(l.payload) : undefined;
      return {
        time: formatRelativeTime(l.ts),
        message: `[${l.event}]`,
        status: this.timelineStatus(l.event),
        detail: payloadStr,
      };
    });
  }

  private scrollToBottom(): void {
    this.updateComplete.then(() => {
      const el = this.shadowRoot?.querySelector(".log-area");
      if (el) el.scrollTop = el.scrollHeight;
    });
  }

  private clearLogs(): void {
    this.logs = [];
    this._buffer = [];
  }

  private get filteredLogs(): LogEntry[] {
    let list = this.logs;
    if (this._level !== "all") {
      list = list.filter((l) => l.event === this._level);
    }
    if (this.filter.trim()) {
      const q = this.filter.toLowerCase();
      list = list.filter(
        (l) =>
          l.event.toLowerCase().includes(q) || JSON.stringify(l.payload).toLowerCase().includes(q),
      );
    }
    return list;
  }

  private _renderHeader(): ReturnType<typeof html> {
    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Logs"
          description="System event log and debugging output"
        ></sc-section-header>
      </sc-page-hero>
    `;
  }

  private _renderSkeleton(): ReturnType<typeof html> {
    return html`
      <div class="controls-sticky">
        <div class="controls">
          <sc-skeleton variant="card" height="2.5rem" style="width: 12rem"></sc-skeleton>
          <sc-skeleton variant="card" height="2.5rem" style="width: 13.75rem"></sc-skeleton>
          <sc-skeleton variant="card" height="2.5rem" style="width: 4rem"></sc-skeleton>
        </div>
      </div>
      <sc-card class="log-card" glass>
        <div class="log-area-wrapper" style="min-height: 12rem">
          <div class="log-area">
            <sc-skeleton
              variant="line"
              style="width: 100%; margin-bottom: var(--sc-space-sm)"
            ></sc-skeleton>
            <sc-skeleton
              variant="line"
              style="width: 90%; margin-bottom: var(--sc-space-sm)"
            ></sc-skeleton>
            <sc-skeleton
              variant="line"
              style="width: 95%; margin-bottom: var(--sc-space-sm)"
            ></sc-skeleton>
            <sc-skeleton
              variant="line"
              style="width: 85%; margin-bottom: var(--sc-space-sm)"
            ></sc-skeleton>
            <sc-skeleton variant="line" style="width: 70%"></sc-skeleton>
          </div>
        </div>
      </sc-card>
    `;
  }

  private _renderControls(): ReturnType<typeof html> {
    return html`
      <div class="controls-sticky" role="toolbar" aria-label="Log filters and actions">
        <div class="controls">
          <sc-segmented-control
            .value=${this._level}
            .options=${LEVEL_OPTIONS}
            @sc-change=${(e: CustomEvent<{ value: string }>) => (this._level = e.detail.value)}
          ></sc-segmented-control>
          <sc-input
            type="text"
            placeholder="Filter..."
            aria-label="Filter log events"
            .value=${this.filter}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
          ></sc-input>
          <sc-badge variant="neutral">${this.filteredLogs.length}</sc-badge>
          <sc-button
            variant="ghost"
            size="sm"
            @click=${this.togglePause}
            aria-label=${this._paused ? "Resume" : "Pause"}
          >
            ${this._paused ? icons.play : icons.pause} ${this._paused ? "Resume" : "Pause"}
          </sc-button>
          <sc-button variant="ghost" size="sm" @click=${this.clearLogs} aria-label="Clear all logs">
            Clear
          </sc-button>
        </div>
      </div>
    `;
  }

  private _renderLogArea(): ReturnType<typeof html> {
    const entries = this.filteredLogs;
    void this._relativeTimeKey;
    const timelineItems = this.toTimelineItems(entries);
    return html`
      <div class="sc-stagger">
        <sc-card class="log-card" glass>
          <div class="log-area-wrapper">
            <div class="log-area" role="log" aria-live="polite">
              ${entries.length === 0
                ? html`
                    <sc-empty-state
                      .icon=${icons["file-text"]}
                      heading="Waiting for events..."
                      description="Logs will stream here in real-time as the system processes requests."
                    ></sc-empty-state>
                  `
                : html`
                    <sc-timeline
                      .items=${timelineItems}
                      .max=${Math.max(entries.length, 10000)}
                    ></sc-timeline>
                  `}
            </div>
          </div>
        </sc-card>
      </div>
    `;
  }

  override render() {
    if (this.error) {
      return html`
        <div class="layout">
          ${this._renderHeader()}
          <sc-empty-state .icon=${icons.warning} heading="Not connected" description=${this.error}>
            <sc-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry logs connection"
              >Retry</sc-button
            >
          </sc-empty-state>
        </div>
      `;
    }
    if (this.loading) {
      return html` <div class="layout">${this._renderHeader()} ${this._renderSkeleton()}</div> `;
    }
    return html`
      <div class="layout">
        ${this._renderHeader()} ${this._renderControls()} ${this._renderLogArea()}
      </div>
    `;
  }
}
