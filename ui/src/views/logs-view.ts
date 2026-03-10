import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayClient } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { EVENT_NAMES } from "../utils.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-input.js";
import "../components/hu-button.js";
import "../components/hu-empty-state.js";
import "../components/hu-card.js";
import "../components/hu-segmented-control.js";
import "../components/hu-badge.js";
import "../components/hu-timeline.js";
import "../components/hu-skeleton.js";
import type { TimelineItem } from "../components/hu-timeline.js";

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

@customElement("hu-logs-view")
export class ScLogsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      view-transition-name: view-logs;
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
      color: var(--hu-text);
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
      margin-bottom: var(--hu-space-md);
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
    }
    .controls-sticky {
      position: sticky;
      top: 0;
      z-index: 1;
      background: var(--hu-bg);
      padding: var(--hu-space-md) 0;
      margin-bottom: var(--hu-space-sm);
      border-bottom: 1px solid var(--hu-border-subtle);
    }
    .controls {
      display: flex;
      gap: var(--hu-space-sm);
      align-items: center;
      flex-wrap: wrap;
    }
    .filter-input {
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font-mono);
      width: 13.75rem;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }
    .filter-input:focus {
      outline: none;
      border-color: var(--hu-accent);
      box-shadow: 0 0 0 var(--hu-space-xs) var(--hu-accent-subtle);
    }
    .filter-input::placeholder {
      color: var(--hu-text-muted);
    }
    .event.chat {
      color: var(--hu-success);
    }
    .event.tool-call {
      color: var(--hu-info);
    }
    .event.error {
      color: var(--hu-error);
    }
    .event.health {
      color: var(--hu-warning);
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
      padding: var(--hu-space-md);
      overflow-y: auto;
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      line-height: 1.6;
      color: var(--hu-text);
    }
    .log-area::-webkit-scrollbar {
      width: var(--hu-space-sm);
    }
    .log-area::-webkit-scrollbar-track {
      background: var(--hu-bg-elevated);
      border-radius: var(--hu-radius-sm);
    }
    .log-area::-webkit-scrollbar-thumb {
      background: var(--hu-border);
      border-radius: var(--hu-radius-sm);
    }
    .log-area::-webkit-scrollbar-thumb:hover {
      background: var(--hu-text-muted);
    }
    .skeleton-control {
      width: 12rem;
    }
    .skeleton-filter {
      width: 13.75rem;
    }
    .skeleton-action {
      width: 4rem;
    }
    .log-area-min {
      min-height: 12rem;
    }
    .skeleton-line {
      width: 100%;
      margin-bottom: var(--hu-space-sm);
    }
    .skeleton-line-90 {
      width: 90%;
      margin-bottom: var(--hu-space-sm);
    }
    .skeleton-line-95 {
      width: 95%;
      margin-bottom: var(--hu-space-sm);
    }
    .skeleton-line-85 {
      width: 85%;
      margin-bottom: var(--hu-space-sm);
    }
    .skeleton-line-70 {
      width: 70%;
    }
    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .header {
        flex-wrap: wrap;
      }
      .controls {
        flex-wrap: wrap;
      }
    }
    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
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

  override updated(): void {
    // Auto-scroll to bottom when new log entries are added
    if (!this._paused && this.filteredLogs.length > 0) {
      this.scrollToBottom();
    }
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

  protected override onGatewaySwapped(
    previous: GatewayClient | null,
    current: GatewayClient,
  ): void {
    previous?.removeEventListener(GatewayClient.EVENT_STATUS, this._logsStatusHandler);
    previous?.removeEventListener(
      GatewayClient.EVENT_GATEWAY,
      this.handleGatewayEvent as EventListener,
    );
    current.addEventListener(GatewayClient.EVENT_STATUS, this._logsStatusHandler);
    current.addEventListener(GatewayClient.EVENT_GATEWAY, this.handleGatewayEvent as EventListener);
    const status = current.status;
    if (status === "connected") {
      this.loading = false;
      this.error = "";
    } else if (status === "disconnected") {
      this.loading = false;
      this.error = "Not connected";
    } else {
      this.loading = true;
      this.error = "";
    }
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

  /** Map log event to hu-timeline status */
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
      if (el) {
        el.scrollTop = el.scrollHeight;
      }
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
      <hu-page-hero role="region" aria-label="Logs">
        <hu-section-header
          heading="Logs"
          description="System event log and debugging output"
        ></hu-section-header>
      </hu-page-hero>
    `;
  }

  private _renderSkeleton(): ReturnType<typeof html> {
    return html`
      <div class="controls-sticky">
        <div class="controls">
          <hu-skeleton variant="card" height="2.5rem" class="skeleton-control"></hu-skeleton>
          <hu-skeleton variant="card" height="2.5rem" class="skeleton-filter"></hu-skeleton>
          <hu-skeleton variant="card" height="2.5rem" class="skeleton-action"></hu-skeleton>
        </div>
      </div>
      <hu-card class="log-card" glass>
        <div class="log-area-wrapper log-area-min">
          <div class="log-area">
            <hu-skeleton variant="line" class="skeleton-line"></hu-skeleton>
            <hu-skeleton variant="line" class="skeleton-line-90"></hu-skeleton>
            <hu-skeleton variant="line" class="skeleton-line-95"></hu-skeleton>
            <hu-skeleton variant="line" class="skeleton-line-85"></hu-skeleton>
            <hu-skeleton variant="line" class="skeleton-line-70"></hu-skeleton>
          </div>
        </div>
      </hu-card>
    `;
  }

  private _renderControls(): ReturnType<typeof html> {
    return html`
      <div class="controls-sticky" role="toolbar" aria-label="Log filters and actions">
        <div class="controls">
          <hu-segmented-control
            .value=${this._level}
            .options=${LEVEL_OPTIONS}
            @hu-change=${(e: CustomEvent<{ value: string }>) => (this._level = e.detail.value)}
          ></hu-segmented-control>
          <hu-input
            type="text"
            placeholder="Filter..."
            aria-label="Filter log events"
            .value=${this.filter}
            @hu-input=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
          ></hu-input>
          <hu-badge variant="neutral">${this.filteredLogs.length}</hu-badge>
          <hu-button
            variant="ghost"
            size="sm"
            @click=${this.togglePause}
            aria-label=${this._paused ? "Resume" : "Pause"}
          >
            ${this._paused ? icons.play : icons.pause} ${this._paused ? "Resume" : "Pause"}
          </hu-button>
          <hu-button variant="ghost" size="sm" @click=${this.clearLogs} aria-label="Clear all logs">
            Clear
          </hu-button>
        </div>
      </div>
    `;
  }

  private _renderLogArea(): ReturnType<typeof html> {
    const entries = this.filteredLogs;
    void this._relativeTimeKey;
    const timelineItems = this.toTimelineItems(entries);
    return html`
      <div class="hu-stagger">
        <hu-card class="log-card" glass>
          <div class="log-area-wrapper">
            <div class="log-area" role="log" aria-live="polite">
              ${entries.length === 0
                ? html`
                    <hu-empty-state
                      .icon=${icons["file-text"]}
                      heading="Waiting for events..."
                      description="Logs will stream here in real-time as the system processes requests."
                    ></hu-empty-state>
                  `
                : html`
                    <hu-timeline
                      .items=${timelineItems}
                      .max=${Math.max(entries.length, 10000)}
                    ></hu-timeline>
                  `}
            </div>
          </div>
        </hu-card>
      </div>
    `;
  }

  override render() {
    if (this.error) {
      return html`
        <div class="layout">
          ${this._renderHeader()}
          <hu-empty-state .icon=${icons.warning} heading="Not connected" description=${this.error}>
            <hu-button
              variant="primary"
              @click=${() => this.load()}
              aria-label="Retry logs connection"
              >Retry</hu-button
            >
          </hu-empty-state>
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
