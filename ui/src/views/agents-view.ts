import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
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
    }
    .header {
      margin-bottom: var(--sc-space-md);
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

  override render() {
    if (this.loading) {
      return html`
        <div class="header"><h2>Agents</h2></div>
        <sc-skeleton
          variant="card"
          height="80px"
          style="margin-bottom: var(--sc-space-lg);"
        ></sc-skeleton>
      `;
    }

    const toolCount = this.capabilities.tools?.length ?? 0;
    const channelCount = this.capabilities.channels?.length ?? 0;

    return html`
      <div class="header">
        <h2>Agents</h2>
      </div>
      ${this.error
        ? html`<sc-empty-state
            icon="⚠️"
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      <sc-card style="margin-bottom: var(--sc-space-lg);">
        <div class="profile-header">
          <span class="profile-title">Agent profile</span>
          <sc-button variant="primary" size="sm" @click=${() => this.dispatchNavigate("config")}>
            Edit Config
          </sc-button>
        </div>
        <div class="profile-grid">
          <div class="profile-item">
            Provider: <strong>${this.config.default_provider ?? "—"}</strong>
          </div>
          <div class="profile-item">
            Model: <strong>${this.config.default_model ?? "—"}</strong>
          </div>
          <div class="profile-item">
            Temperature: <strong>${this.config.temperature ?? "—"}</strong>
          </div>
          <div class="profile-item">
            Max tokens: <strong>${this.config.max_tokens ?? "—"}</strong>
          </div>
          <div class="profile-item">Tools: <strong>${toolCount}</strong></div>
          <div class="profile-item">Channels: <strong>${channelCount}</strong></div>
        </div>
      </sc-card>

      <div class="section-title">Active Sessions</div>
      <div class="sessions-list">
        ${this.sessions.length === 0
          ? html`
              <sc-empty-state
                icon="🗨️"
                heading="No active sessions"
                description="Start a conversation to see sessions here."
              ></sc-empty-state>
            `
          : this.sessions.map(
              (s) => html`
                <sc-card>
                  <div class="session-row">
                    <div class="session-info">
                      <div class="session-key">${s.label ?? s.key ?? "—"}</div>
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

      <div class="stats-bar">
        <span class="stats-item"
          >Sessions: <span class="stats-value">${this.sessions.length}</span></span
        >
        <span class="stats-item"
          >Total turns: <span class="stats-value">${this.totalTurns}</span></span
        >
        <span class="stats-item">Channels: <span class="stats-value">${channelCount}</span></span>
        <span class="stats-item">Tools: <span class="stats-value">${toolCount}</span></span>
      </div>
    `;
  }
}
