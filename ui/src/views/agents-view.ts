import { html, css } from "lit";
import { customElement, state } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

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
      margin-bottom: 1rem;
    }
    h2 {
      margin: 0;
      font-size: 1.25rem;
      font-weight: 600;
      color: var(--sc-text);
    }
    .profile-card {
      padding: 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      margin-bottom: 1.5rem;
    }
    .profile-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 0.75rem;
    }
    .profile-title {
      font-size: 0.9375rem;
      font-weight: 600;
      color: var(--sc-text);
    }
    .edit-btn {
      padding: 0.375rem 0.75rem;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-size: 0.8125rem;
      cursor: pointer;
    }
    .edit-btn:hover {
      background: var(--sc-accent-hover);
    }
    .profile-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
      gap: 0.75rem;
      font-size: 0.8125rem;
    }
    .profile-item {
      color: var(--sc-text-muted);
    }
    .profile-item strong {
      color: var(--sc-text);
    }
    .section-title {
      font-size: 0.9375rem;
      font-weight: 600;
      color: var(--sc-text);
      margin: 1rem 0 0.5rem;
    }
    .sessions-list {
      display: flex;
      flex-direction: column;
      gap: 0.5rem;
      margin-bottom: 1.5rem;
    }
    .session-card {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0.75rem 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      gap: 1rem;
      flex-wrap: wrap;
    }
    .session-info {
      flex: 1;
      min-width: 0;
    }
    .session-key {
      font-family: var(--sc-font-mono);
      font-size: 0.8125rem;
      color: var(--sc-text);
      margin-bottom: 0.25rem;
    }
    .session-meta {
      font-size: 0.75rem;
      color: var(--sc-text-muted);
    }
    .resume-btn {
      padding: 0.375rem 0.75rem;
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-size: 0.8125rem;
      cursor: pointer;
    }
    .resume-btn:hover {
      background: var(--sc-accent-hover);
    }
    .stats-bar {
      display: flex;
      flex-wrap: wrap;
      gap: 1.5rem;
      padding: 0.75rem 0;
      font-size: 0.8125rem;
      color: var(--sc-text-muted);
      border-top: 1px solid var(--sc-border);
    }
    .stats-item {
      display: flex;
      align-items: center;
      gap: 0.25rem;
    }
    .stats-value {
      font-weight: 600;
      color: var(--sc-text);
      font-variant-numeric: tabular-nums;
    }
    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-surface) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
      animation: sc-shimmer 1.5s ease-in-out infinite;
      border-radius: var(--sc-radius);
    }
    .skeleton-line {
      height: 1rem;
      margin-bottom: 0.75rem;
      border-radius: 4px;
    }
    .skeleton-card {
      height: 5rem;
      margin-bottom: 0.75rem;
    }
    .error {
      color: var(--sc-error);
      font-size: 0.875rem;
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
        <div class="profile-card skeleton skeleton-card"></div>
      `;
    }

    const toolCount = this.capabilities.tools?.length ?? 0;
    const channelCount = this.capabilities.channels?.length ?? 0;

    return html`
      <div class="header">
        <h2>Agents</h2>
      </div>
      ${this.error ? html`<p class="error">${this.error}</p>` : ""}
      <div class="profile-card">
        <div class="profile-header">
          <span class="profile-title">Agent profile</span>
          <button class="edit-btn" @click=${() => this.dispatchNavigate("config")}>
            Edit Config
          </button>
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
      </div>

      <div class="section-title">Active Sessions</div>
      <div class="sessions-list">
        ${this.sessions.length === 0
          ? html`<p class="loading">No active sessions</p>`
          : this.sessions.map(
              (s) => html`
                <div class="session-card">
                  <div class="session-info">
                    <div class="session-key">${s.label ?? s.key ?? "—"}</div>
                    <div class="session-meta">
                      ${s.turn_count ?? 0} turns · Last: ${formatDate(s.last_active)}
                    </div>
                  </div>
                  <button
                    class="resume-btn"
                    @click=${() => this.dispatchNavigate("chat:" + (s.key ?? "default"))}
                  >
                    Resume
                  </button>
                </div>
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
