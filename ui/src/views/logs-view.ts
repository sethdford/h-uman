import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayClient } from "../gateway.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { EVENT_NAMES } from "../utils.js";

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
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-md);
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .controls {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .filter-input {
      padding: 0.5rem 0.75rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: 0.875rem;
      font-family: var(--sc-font-mono);
      width: 220px;
      transition:
        border-color 0.2s var(--sc-ease-out),
        box-shadow 0.2s var(--sc-ease-out);
    }
    .filter-input:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    .filter-input::placeholder {
      color: var(--sc-text-muted);
    }
    .btn {
      padding: 0.5rem 1rem;
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      cursor: pointer;
      font-size: 0.875rem;
      font-weight: var(--sc-weight-medium);
      transition: background 0.2s var(--sc-ease-out);
    }
    .btn:hover {
      background: var(--sc-border);
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
      box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.2);
    }
    .log-area::-webkit-scrollbar {
      width: 8px;
    }
    .log-area::-webkit-scrollbar-track {
      background: var(--sc-bg-elevated);
      border-radius: 4px;
    }
    .log-area::-webkit-scrollbar-thumb {
      background: var(--sc-border);
      border-radius: 4px;
    }
    .log-area::-webkit-scrollbar-thumb:hover {
      background: var(--sc-text-muted);
    }
    .log-line {
      margin-bottom: var(--sc-space-sm);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      background: rgba(0, 0, 0, 0.2);
      word-break: break-all;
    }
    .log-ts {
      color: var(--sc-text-muted);
      margin-right: var(--sc-space-sm);
    }
    .event {
      font-weight: var(--sc-weight-semibold);
      margin-right: var(--sc-space-sm);
    }
  `;

  @state() private logs: LogEntry[] = [];
  @state() private filter = "";
  @state() private logAreaRef: HTMLDivElement | null = null;

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

  private eventColor(event: string): string {
    switch (event) {
      case EVENT_NAMES.CHAT:
        return "var(--sc-success)";
      case EVENT_NAMES.TOOL_CALL:
        return "var(--sc-info)";
      case EVENT_NAMES.ERROR:
        return "var(--sc-error)";
      case EVENT_NAMES.HEALTH:
        return "var(--sc-warning)";
      default:
        return "var(--sc-text)";
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

  override render() {
    const entries = this.filteredLogs;

    return html`
      <div class="header">
        <h2>Logs</h2>
        <div class="controls">
          <input
            type="text"
            class="filter-input"
            placeholder="Filter..."
            .value=${this.filter}
            @input=${(e: Event) => (this.filter = (e.target as HTMLInputElement).value)}
          />
          <button class="btn" @click=${this.clearLogs}>Clear</button>
        </div>
      </div>
      <div class="log-area" role="log">
        ${entries.length === 0
          ? html`<span style="color: var(--sc-text-muted)">Listening for gateway events...</span>`
          : entries.map(
              (l) => html`
                <div class="log-line">
                  <span class="log-ts">${l.ts}</span>
                  <span class="event" style="color: ${this.eventColor(l.event)}">[${l.event}]</span>
                  ${JSON.stringify(l.payload)}
                </div>
              `,
            )}
      </div>
    `;
  }
}
