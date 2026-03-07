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

interface LogEntry {
  ts: string;
  event: string;
  payload: Record<string, unknown>;
}

@customElement("sc-logs-view")
export class ScLogsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 960px;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
    }
    .controls {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .filter-input {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font-mono);
      width: 220px;
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .filter-input:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
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
    .log-area-wrapper {
      position: relative;
    }
    .log-area-wrapper::after {
      content: "";
      position: absolute;
      bottom: 0;
      left: 0;
      right: 0;
      height: 2rem;
      background: linear-gradient(to top, var(--sc-bg-inset) 20%, transparent);
      pointer-events: none;
      border-radius: 0 0 var(--sc-radius) var(--sc-radius);
    }
    .log-area {
      background: var(--sc-bg-inset);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      padding: var(--sc-space-md);
      height: 400px;
      overflow-y: auto;
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      line-height: 1.6;
      color: var(--sc-text);
      box-shadow: var(--sc-shadow-sm);
    }
    .log-area::-webkit-scrollbar {
      width: 8px;
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
    .log-line {
      margin-bottom: var(--sc-space-sm);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      word-break: break-all;
    }
    .log-line:nth-child(odd) {
      background: var(--sc-bg-surface);
    }
    .log-line:nth-child(even) {
      background: var(--sc-bg-inset);
    }
    .log-ts {
      color: var(--sc-text-muted);
      margin-right: var(--sc-space-sm);
      font-variant-numeric: tabular-nums;
    }
    .event {
      font-weight: var(--sc-weight-semibold);
      margin-right: var(--sc-space-sm);
    }
    @media (max-width: 768px) {
      .header {
        flex-wrap: wrap;
      }
      .controls {
        flex-wrap: wrap;
      }
    }
    @media (max-width: 480px) {
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
  @state() private filter = "";

  protected override async load(): Promise<void> {
    this.setupListener();
  }

  override disconnectedCallback(): void {
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
    this.logs = [
      ...this.logs,
      {
        ts: new Date().toISOString(),
        event: ev.event,
        payload: ev.payload ?? {},
      },
    ];
    this.requestUpdate();
    this.scrollToBottom();
  };

  private eventClass(event: string): string {
    switch (event) {
      case EVENT_NAMES.CHAT:
        return "chat";
      case EVENT_NAMES.TOOL_CALL:
        return "tool-call";
      case EVENT_NAMES.ERROR:
        return "error";
      case EVENT_NAMES.HEALTH:
        return "health";
      default:
        return "";
    }
  }

  private scrollToBottom(): void {
    this.updateComplete.then(() => {
      const el = this.shadowRoot?.querySelector(".log-area");
      if (el) el.scrollTop = el.scrollHeight;
    });
  }

  private clearLogs(): void {
    this.logs = [];
  }

  private get filteredLogs(): LogEntry[] {
    if (!this.filter.trim()) return this.logs;
    const q = this.filter.toLowerCase();
    return this.logs.filter(
      (l) =>
        l.event.toLowerCase().includes(q) || JSON.stringify(l.payload).toLowerCase().includes(q),
    );
  }

  private _renderHeader(): ReturnType<typeof html> {
    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Logs"
          description="System event log and debugging output"
        ></sc-section-header>
      </sc-page-hero>
      <div class="header">
        <div class="controls">
          <sc-input
            type="text"
            placeholder="Filter..."
            aria-label="Filter log events"
            .value=${this.filter}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.filter = e.detail.value)}
          ></sc-input>
          <sc-button variant="ghost" size="sm" @click=${this.clearLogs} aria-label="Clear all logs">
            Clear
          </sc-button>
        </div>
      </div>
    `;
  }

  private _renderLogArea(): ReturnType<typeof html> {
    const entries = this.filteredLogs;
    return html`
      <sc-card class="log-card">
        <div class="log-area-wrapper">
          <div class="log-area sc-stagger" role="log" aria-live="polite">
            ${entries.length === 0
              ? html`
                  <sc-empty-state
                    .icon=${icons["file-text"]}
                    heading="No logs yet"
                    description="Logs will appear here when the system starts processing."
                  ></sc-empty-state>
                `
              : entries.map(
                  (l) => html`
                    <div class="log-line">
                      <span class="log-ts">${l.ts}</span>
                      <span class="event ${this.eventClass(l.event)}">[${l.event}]</span>
                      ${JSON.stringify(l.payload)}
                    </div>
                  `,
                )}
          </div>
        </div>
      </sc-card>
    `;
  }

  override render() {
    return html` ${this._renderHeader()} ${this._renderLogArea()} `;
  }
}
