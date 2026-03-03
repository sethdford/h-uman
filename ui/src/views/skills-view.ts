import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";

interface Skill {
  name: string;
  description?: string;
  enabled?: boolean;
}

@customElement("sc-skills-view")
export class ScSkillsView extends GatewayAwareLitElement {
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
      gap: var(--sc-space-md);
    }
    h2 {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .install-row {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .install-row input {
      padding: 0.5rem 0.75rem;
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: 0.875rem;
      width: 200px;
    }
    .skills-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-md);
    }
    .skill-card-inner {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .skill-name {
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .skill-desc {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      flex: 1;
    }
    .skill-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
    }
    .toggle {
      position: relative;
      width: 44px;
      height: 24px;
      background: var(--sc-border);
      border-radius: 12px;
      cursor: pointer;
      transition: background 0.2s;
    }
    .toggle.enabled {
      background: var(--sc-accent);
    }
    .toggle::after {
      content: "";
      position: absolute;
      top: 2px;
      left: 2px;
      width: 20px;
      height: 20px;
      background: white;
      border-radius: 50%;
      transition: transform 0.2s;
    }
    .toggle.enabled::after {
      transform: translateX(20px);
    }
    .status {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .grid-full {
      grid-column: 1 / -1;
    }
  `;

  @state() private skills: Skill[] = [];
  @state() private loading = false;
  @state() private error = "";
  @state() private installUrl = "";

  protected override async load(): Promise<void> {
    await this.loadSkills();
  }

  private async loadSkills(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    try {
      const res = (await gw.request<{ skills?: Skill[] }>("skills.list", {})) as
        | { skills?: Skill[] }
        | { result?: { skills?: Skill[] } };
      const skills =
        (res && "skills" in res && res.skills) ||
        (res && "result" in res && res.result?.skills) ||
        [];
      this.skills = Array.isArray(skills) ? skills : [];
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load skills";
      this.skills = [];
    } finally {
      this.loading = false;
    }
  }

  private async toggleSkill(skill: Skill): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    const enable = !skill.enabled;
    this.error = "";
    try {
      await gw.request(enable ? "skills.enable" : "skills.disable", {
        name: skill.name,
      });
      skill.enabled = enable;
      this.requestUpdate();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to update skill";
    }
  }

  private async installSkill(): Promise<void> {
    const gw = this.gateway;
    if (!gw || !this.installUrl.trim()) return;
    this.error = "";
    try {
      await gw.request("skills.install", {
        name: this.installUrl.trim(),
      });
      this.installUrl = "";
      await this.loadSkills();
    } catch (e) {
      this.error =
        e instanceof Error
          ? e.message
          : "Install not supported or failed (skills.install may be unavailable)";
    }
  }

  override render() {
    return html`
      <div class="header">
        <h2>Skills</h2>
        <div class="install-row">
          <input
            type="url"
            placeholder="https://..."
            .value=${this.installUrl}
            @input=${(e: Event) => (this.installUrl = (e.target as HTMLInputElement).value)}
          />
          <sc-button variant="primary" @click=${this.installSkill}>Install</sc-button>
        </div>
      </div>
      ${this.error
        ? html`<sc-empty-state
            icon="⚠️"
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading
        ? html`
            <div class="skills-grid">
              <sc-skeleton variant="card" height="100px"></sc-skeleton>
              <sc-skeleton variant="card" height="100px"></sc-skeleton>
              <sc-skeleton variant="card" height="100px"></sc-skeleton>
            </div>
          `
        : this.skills.length === 0
          ? html`
              <div class="skills-grid">
                <div class="grid-full">
                  <sc-empty-state
                    icon="🧩"
                    heading="No skills installed"
                    description="Install skills to extend your agent's capabilities."
                  ></sc-empty-state>
                </div>
              </div>
            `
          : html`
              <div class="skills-grid">
                ${this.skills.map(
                  (skill) => html`
                    <sc-card>
                      <div class="skill-card-inner">
                        <div class="skill-name">${skill.name}</div>
                        <div class="skill-desc">${skill.description ?? "No description"}</div>
                        <div class="skill-footer">
                          <span class="status">
                            ${skill.enabled !== false ? "Enabled" : "Disabled"}
                          </span>
                          <div
                            class="toggle ${skill.enabled !== false ? "enabled" : ""}"
                            @click=${() => this.toggleSkill(skill)}
                            role="switch"
                            aria-checked=${skill.enabled !== false}
                          ></div>
                        </div>
                      </div>
                    </sc-card>
                  `,
                )}
              </div>
            `}
    `;
  }
}
