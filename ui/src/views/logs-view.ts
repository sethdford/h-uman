import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { ref } from "lit/directives/ref.js";
import { staggerMotion9Styles } from "../styles/scroll-entrance.js";
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
import "../components/hu-skeleton.js";
import "../components/hu-timeline.js";
import type { TimelineItem } from "../components/hu-timeline.js";
import { ScToast } from "../components/hu-toast.js";

const ROW_HEIGHT = 32;
const WINDOW_SIZE = 100;
const BUFFER = 50;

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

function highlightText(text: string, query: string): (string | ReturnType<typeof html>)[] {
  if (!query.trim()) return [text];
  const q = query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const re = new RegExp(`(${q})`, "gi");
  const parts = text.split(re);
  return parts.map((p, i) => (i % 2 === 1 ? html`<mark class="mark">${p}</mark>` : p));
}

@customElement("hu-logs-view")
export class ScLogsView extends GatewayAwareLitElement {
  static override styles = [
    staggerMotion9Styles,
    css`
      :host {
        view-transition-name: view-logs;
        display: flex;
        width: 100%;
        flex-direction: column;
        contain: layout style;
        container-type: inline-size;
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
        background: var(--hu-surface-container);
        border: 1px solid var(--hu-border);
        border-radius: var(--hu-radius);
        color: var(--hu-text);
        font-size: var(--hu-text-sm);
        font-family: var(--hu-font-mono);
        width: min(13.75rem, 100%);
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
      .mark {
        background: var(--hu-accent-subtle);
        border-radius: var(--hu-radius-xs);
        padding: 0 var(--hu-space-2xs);
      }
      .log-row {
        display: flex;
        align-items: flex-start;
        gap: var(--hu-space-md);
        min-height: 32px; /* ROW_HEIGHT for virtual scroll */
        padding: var(--hu-space-2xs) 0;
        font-size: var(--hu-text-sm);
        line-height: 1.5;
      }
      .log-row .level-dot {
        flex-shrink: 0;
        width: 6px;
        height: 6px;
        border-radius: var(--hu-radius-full);
        margin-top: 0.4em;
      }
      .log-row .time {
        flex-shrink: 0;
        font-variant-numeric: tabular-nums;
        color: var(--hu-text-faint);
      }
      .log-row .content {
        flex: 1;
        min-width: 0;
        word-break: break-word;
        color: var(--hu-text-secondary);
      }
      .sentinel {
        height: 1px;
        width: 100%;
        pointer-events: none;
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
        line-height: 1.5;
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
        width: min(13.75rem, 100%);
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
      @container (max-width: 48rem) /* cq-medium */ {
        .header {
          flex-wrap: wrap;
        }
        .controls {
          flex-wrap: wrap;
        }
      }
      @container (max-width: 30rem) /* cq-sm */ {
        .filter-input {
          width: 100%;
        }
        .btn-label {
          display: none;
        }
      }
      @media (prefers-reduced-motion: reduce) {
        .filter-input,
        .log-area {
          transition: none !important;
        }
      }
    `,
  ];

  @state() private logs: LogEntry[] = [];
  @state() private _buffer: LogEntry[] = [];
  @state() private filter = "";
  @state() private _level: string = "all";
  @state() private _paused = false;
  @state() private _relativeTimeKey = 0;
  @state() private loading = true;
  @state() private error = "";
  @state() private _visibleRange: { start: number; end: number } = {
    start: 0,
    end: WINDOW_SIZE + BUFFER * 2,
  };

  private _scrollContainer: HTMLDivElement | null = null;
  private _topSentinel: HTMLDivElement | null = null;
  private _bottomSentinel: HTMLDivElement | null = null;
  private _observer: IntersectionObserver | null = null;

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

  override willUpdate(): void {
    const entries = this.filteredLogs;
    const total = entries.length;
    if (total === 0) return;
    const { start, end } = this._visibleRange;
    if (!this._paused) {
      const newEnd = total;
      const newStart = Math.max(0, newEnd - WINDOW_SIZE - BUFFER * 2);
      if (start !== newStart || end !== newEnd) {
        this._visibleRange = { start: newStart, end: newEnd };
      }
    } else {
      const clampedEnd = Math.min(end, total);
      const clampedStart = Math.min(start, Math.max(0, clampedEnd - 1));
      if (clampedStart !== start || clampedEnd !== end) {
        this._visibleRange = { start: clampedStart, end: clampedEnd };
      }
    }
  }

  override updated(): void {
    if (!this._paused) {
      this.scrollToBottom();
    }
  }

  override disconnectedCallback(): void {
    this._observer?.disconnect();
    this._observer = null;
    this.gateway?.removeEventListener(GatewayClient.EVENT_STATUS, this._logsStatusHandler);
    if (this._tsInterval) {
      clearInterval(this._tsInterval);
      this._tsInterval = null;
    }
    this.removeListener();
    super.disconnectedCallback();
  }

  override firstUpdated(): void {
    this._setupIntersectionObserver();
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

  private _getLevelFromEntry(entry: LogEntry): string {
    const p = entry.payload as { level?: string; severity?: string };
    if (p?.level) return String(p.level).toUpperCase();
    if (p?.severity) return String(p.severity).toUpperCase();
    if (entry.event === EVENT_NAMES.ERROR) return "ERROR";
    return "INFO";
  }

  private _getLevelColor(level: string): string {
    const u = level.toUpperCase();
    if (u === "ERROR" || u === "FATAL") return "var(--hu-error)";
    if (u === "WARN" || u === "WARNING") return "var(--hu-accent-secondary)";
    if (u === "INFO") return "var(--hu-text-secondary)";
    if (u === "DEBUG") return "var(--hu-text-tertiary)";
    return "var(--hu-text-secondary)";
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

  private _setupIntersectionObserver(): void {
    if (!this._scrollContainer || !this._topSentinel || !this._bottomSentinel) return;
    const total = this.filteredLogs.length;
    if (total === 0) return;
    this._observer?.disconnect();
    this._observer = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (!e.isIntersecting) continue;
          const totalNow = this.filteredLogs.length;
          if (e.target === this._topSentinel) {
            this._visibleRange = {
              start: Math.max(0, this._visibleRange.start - BUFFER),
              end: this._visibleRange.end,
            };
          } else if (e.target === this._bottomSentinel) {
            this._visibleRange = {
              start: this._visibleRange.start,
              end: Math.min(totalNow, this._visibleRange.end + BUFFER),
            };
          }
        }
      },
      { root: this._scrollContainer, rootMargin: "0px", threshold: 0 },
    );
    if (this._topSentinel) this._observer.observe(this._topSentinel);
    if (this._bottomSentinel) this._observer.observe(this._bottomSentinel);
  }

  private _exportLogs(): void {
    const entries = this.filteredLogs;
    const lines = entries.map((l) => {
      const payloadStr = Object.keys(l.payload).length > 0 ? JSON.stringify(l.payload) : "";
      return `${l.ts} [${l.event}] ${payloadStr}`.trim();
    });
    const text = lines.join("\n");
    const blob = new Blob([text], { type: "text/plain" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `logs-${new Date().toISOString().slice(0, 10)}.txt`;
    a.click();
    URL.revokeObjectURL(url);
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

  /** Public method for command palette / Ctrl+Shift+E */
  exportLogs(): void {
    const data = this.filteredLogs;
    const blob = new Blob([JSON.stringify(data, null, 2)], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `logs-${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
    ScToast.show({ message: "Logs exported", variant: "success" });
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
          <div class="log-area hu-stagger-motion9">
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
            class="filter-input"
            type="text"
            placeholder="Search logs..."
            aria-label="Search log events and payload"
            .value=${this.filter}
            @hu-input=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
          ></hu-input>
          <hu-badge variant="neutral">${this.filteredLogs.length}</hu-badge>
          <hu-button
            variant="ghost"
            size="sm"
            ?disabled=${this.filteredLogs.length === 0}
            @click=${this._exportLogs}
            aria-label="Export logs to file"
          >
            ${icons.export}<span class="btn-label"> Export</span>
          </hu-button>
          <hu-button
            variant="ghost"
            size="sm"
            @click=${this.togglePause}
            aria-label=${this._paused ? "Resume" : "Pause"}
          >
            ${this._paused ? icons.play : icons.pause}<span class="btn-label">
              ${this._paused ? "Resume" : "Pause"}</span
            >
          </hu-button>
          <hu-button variant="ghost" size="sm" @click=${this.clearLogs} aria-label="Clear all logs">
            ${icons.trash}<span class="btn-label"> Clear</span>
          </hu-button>
        </div>
      </div>
    `;
  }

  private _renderLogRow(entry: LogEntry, index: number): ReturnType<typeof html> {
    void index;
    const level = this._getLevelFromEntry(entry);
    const color = this._getLevelColor(level);
    const payloadStr = Object.keys(entry.payload).length > 0 ? JSON.stringify(entry.payload) : "";
    const fullLine = `[${entry.event}] ${payloadStr}`.trim();
    const searchParts = highlightText(fullLine, this.filter.trim());
    return html`
      <div class="log-row">
        <span class="level-dot" style="background: ${color}"></span>
        <span class="time">${formatRelativeTime(entry.ts)}</span>
        <span class="content">${searchParts}</span>
      </div>
    `;
  }

  private _renderLogArea(): ReturnType<typeof html> {
    const entries = this.filteredLogs;
    void this._relativeTimeKey;
    const total = entries.length;

    if (total === 0) {
      return html`
        <hu-card class="log-card" glass>
          <div class="log-area-wrapper">
            <div class="log-area hu-stagger-motion9" role="log" aria-live="polite">
              <hu-empty-state
                .icon=${icons["file-text"]}
                heading="Waiting for events..."
                description="Logs will stream here in real-time as the system processes requests."
              ></hu-empty-state>
            </div>
          </div>
        </hu-card>
      `;
    }

    const { start, end } = this._visibleRange;
    const clampedStart = Math.max(0, Math.min(start, total - 1));
    const clampedEnd = Math.min(total, Math.max(clampedStart, end));
    const visibleEntries = entries.slice(clampedStart, clampedEnd);
    const topHeight = clampedStart * ROW_HEIGHT;
    const bottomHeight = (total - clampedEnd) * ROW_HEIGHT;

    return html`
      <hu-card class="log-card" glass>
        <div class="log-area-wrapper">
          <div
            class="log-area hu-stagger-motion9"
            role="log"
            aria-live="polite"
            aria-label="${total} log entries"
            ${ref((el) => {
              this._scrollContainer = el as HTMLDivElement;
              if (this._scrollContainer && this._topSentinel && this._bottomSentinel) {
                this.updateComplete.then(() => this._setupIntersectionObserver());
              }
            })}
          >
            <div style="height: ${topHeight}px" class="sentinel-spacer" aria-hidden="true"></div>
            <div
              class="sentinel"
              ${ref((el) => {
                this._topSentinel = el as HTMLDivElement;
                if (this._scrollContainer && this._topSentinel && this._bottomSentinel) {
                  this.updateComplete.then(() => this._setupIntersectionObserver());
                }
              })}
            ></div>
            ${visibleEntries.map((e, i) => this._renderLogRow(e, clampedStart + i))}
            <div
              class="sentinel"
              ${ref((el) => {
                this._bottomSentinel = el as HTMLDivElement;
                if (this._scrollContainer && this._topSentinel && this._bottomSentinel) {
                  this.updateComplete.then(() => this._setupIntersectionObserver());
                }
              })}
            ></div>
            <div style="height: ${bottomHeight}px" class="sentinel-spacer" aria-hidden="true"></div>
          </div>
        </div>
      </hu-card>
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
