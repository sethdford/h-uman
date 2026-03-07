import { LitElement } from "lit";
import type { GatewayClient } from "./gateway.js";
import { getGateway } from "./gateway-provider.js";

/**
 * Base class for views that depend on gateway connectivity.
 * Calls `load()` automatically once the gateway is connected,
 * and re-calls it on reconnect.
 *
 * Subclasses can set `autoRefreshInterval` (ms) to enable periodic refresh.
 * `lastLoadedAt` tracks when data was last fetched for staleness indicators.
 */
export class GatewayAwareLitElement extends LitElement {
  private _statusHandler = ((e: CustomEvent<string>) => {
    if (e.detail === "connected") this._doLoad();
  }) as EventListener;

  private _refreshTimer: ReturnType<typeof setInterval> | null = null;

  /** Override in subclass to enable auto-refresh (milliseconds). 0 = disabled. */
  protected autoRefreshInterval = 0;

  /** Timestamp of last successful load. Subclasses can use for staleness UI. */
  protected lastLoadedAt = 0;

  protected get gateway(): GatewayClient | null {
    return getGateway();
  }

  /** Seconds since last load, for staleness display. */
  protected get staleness(): number {
    if (!this.lastLoadedAt) return 0;
    return Math.round((Date.now() - this.lastLoadedAt) / 1000);
  }

  /** Human-readable staleness string. */
  protected get stalenessLabel(): string {
    const s = this.staleness;
    if (s === 0) return "Just now";
    if (s < 60) return `${s}s ago`;
    const m = Math.floor(s / 60);
    return `${m}m ago`;
  }

  override connectedCallback(): void {
    super.connectedCallback();
    const gw = this.gateway;
    if (gw) {
      gw.addEventListener("status", this._statusHandler);
      if (gw.status === "connected") {
        this._doLoad();
      }
    }
    this._startAutoRefresh();
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this.gateway?.removeEventListener("status", this._statusHandler);
    this._stopAutoRefresh();
  }

  private async _doLoad(): Promise<void> {
    try {
      await this.load();
      this.lastLoadedAt = Date.now();
    } catch {
      /* leave lastLoadedAt unchanged on failure */
    }
  }

  private _startAutoRefresh(): void {
    this._stopAutoRefresh();
    if (this.autoRefreshInterval > 0) {
      this._refreshTimer = setInterval(() => {
        if (this.gateway?.status === "connected") {
          this._doLoad();
          this.requestUpdate();
        }
      }, this.autoRefreshInterval);
    }
  }

  private _stopAutoRefresh(): void {
    if (this._refreshTimer) {
      clearInterval(this._refreshTimer);
      this._refreshTimer = null;
    }
  }

  protected load(): void | Promise<void> {}
}
