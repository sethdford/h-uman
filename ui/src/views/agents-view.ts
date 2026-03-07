import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface ConfigData {
  default_provider?: string;
  default_model?: string;
  temperature?: number;
  max_tokens?: number;
}

interface Session {
  key?: string;
  label?: string;
  turn_count?: number;
  last_active?: number;
  created_at?: number;
}

interface Capabilities {
  tools?: unknown[];
  channels?: unknown[];
  providers?: unknown[];
}

@customElement("sc-agents-view")
export class ScAgentsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      color: var(--sc-text);
      max-width: 1200px;
    }
    .header {
      margin-bottom: var(--sc-space-xl);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .profile-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-sm);
    }
    .profile-title {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .profile-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
      gap: var(--sc-space-sm);
      font-size: var(--sc-text-sm);
    }
    .profile-item {
      color: var(--sc-text-muted);
    }
    .profile-item strong {
      color: var(--sc-text);
    }
    .section-title {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: var(--sc-space-md) 0 var(--sc-space-sm);
    }
    .sessions-list {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-lg);
    }
    .session-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--sc-space-md);
      flex-wrap: wrap;
    }
    .session-info {
      flex: 1;
      min-width: 0;
    }
    .session-key {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-xs);
    }
    .session-meta {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .stats-bar {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-lg);
      padding: var(--sc-space-sm) 0;
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      border-top: 1px solid var(--sc-border);
    }
    .stats-item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
    }
    .stats-value {
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      font-variant-numeric: tabular-nums;
    }
    @media (max-width: 768px) {
      .profile-grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    .skeleton-spacer {
      margin-bottom: var(--sc-space-lg);
    }
    .card-spacer {
      margin-bottom: var(--sc-space-2xl);
    }
    @media (max-width: 480px) {
      .profile-grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private config: ConfigData = {};
  @state() private sessions: Session[] = [];
  @state() private capabilities: Capabilities = {};
  @state() private loading = false;
  @state() private error = "";

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const [cfg, sess, caps] = await Promise.all([
        gw.request<Partial<ConfigData>>("config.get", {}),
        gw.request<{ sessions?: Session[] }>("sessions.list", {}),
        gw.request<Capabilities>("capabilities", {}),
      ]);
      this.config = {
        default_provider: cfg?.default_provider ?? "",
        default_model: cfg?.default_model ?? "",
        temperature: cfg?.temperature ?? 0.7,
        max_tokens: cfg?.max_tokens ?? 0,
      };
      this.sessions = sess?.sessions ?? [];
      this.capabilities = {
        tools: caps?.tools ?? [],
        channels: caps?.channels ?? [],
        providers: caps?.providers ?? [],
      };
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load";
    } finally {
      this.loading = false;
    }
  }

  private dispatchNavigate(tab: string): void {
    this.dispatchEvent(
      new CustomEvent("navigate", {
        detail: tab,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private get totalTurns(): number {
    return this.sessions.reduce((s, x) => s + (x.turn_count ?? 0), 0);
  }

  private get _toolCount(): number {
    const t = this.capabilities.tools;
    return typeof t === "number" ? t : Array.isArray(t) ? t.length : 0;
  }

  private get _channelCount(): number {
    const ch = this.capabilities.channels;
    return typeof ch === "number" ? ch : Array.isArray(ch) ? ch.length : 0;
  }

  override render() {
    if (this.loading) return this._renderSkeleton();
    return html`
      <div class="header"><h2>Agents</h2></div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this._renderProfile()} ${this._renderSessions()} ${this._renderStats()}
    `;
  }

  private _renderSkeleton() {
    return html`
      <div class="header"><h2>Agents</h2></div>
      <sc-skeleton variant="card" height="80px" class="skeleton-spacer"></sc-skeleton>
    `;
  }

  private _renderProfile() {
    return html`
      <sc-card class="card-spacer">
        <div class="profile-header">
          <span class="profile-title">Agent profile</span>
          <sc-button variant="primary" size="sm" @click=${() => this.dispatchNavigate("config")}>
            Edit Config
          </sc-button>
        </div>
        <div class="profile-grid">
          <div class="profile-item">
            Provider: <strong>${this.config.default_provider ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Model: <strong>${this.config.default_model ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Temperature: <strong>${this.config.temperature ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">
            Max tokens: <strong>${this.config.max_tokens ?? "\u2014"}</strong>
          </div>
          <div class="profile-item">Tools: <strong>${this._toolCount}</strong></div>
          <div class="profile-item">Channels: <strong>${this._channelCount}</strong></div>
        </div>
      </sc-card>
    `;
  }

  private _renderSessions() {
    return html`
      <div class="section-title">Active Sessions</div>
      <div class="sessions-list sc-stagger">
        ${this.sessions.length === 0
          ? html`
              <sc-empty-state
                .icon=${icons["chat-circle"]}
                heading="No active sessions"
                description="Start a conversation to see sessions here."
              ></sc-empty-state>
            `
          : this.sessions.map(
              (s) => html`
                <sc-card>
                  <div class="session-row">
                    <div class="session-info">
                      <div class="session-key">${s.label ?? s.key ?? "\u2014"}</div>
                      <div class="session-meta">
                        ${s.turn_count ?? 0} turns · Last: ${formatDate(s.last_active)}
                      </div>
                    </div>
                    <sc-button
                      variant="primary"
                      size="sm"
                      @click=${() => this.dispatchNavigate("chat:" + (s.key ?? "default"))}
                    >
                      Resume
                    </sc-button>
                  </div>
                </sc-card>
              `,
            )}
      </div>
    `;
  }

  private _renderStats() {
    return html`
      <div class="stats-bar">
        <span class="stats-item"
          >Sessions: <span class="stats-value">${this.sessions.length}</span></span
        >
        <span class="stats-item"
          >Total turns: <span class="stats-value">${this.totalTurns}</span></span
        >
        <span class="stats-item"
          >Channels: <span class="stats-value">${this._channelCount}</span></span
        >
        <span class="stats-item">Tools: <span class="stats-value">${this._toolCount}</span></span>
      </div>
    `;
  }
}
