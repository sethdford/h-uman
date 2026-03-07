import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-card.js";
import "../components/sc-input.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-button.js";
import "../components/sc-switch.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-badge.js";

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
      max-width: 1200px;
    }
    .install-row {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .install-row input {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
      width: 200px;
    }
    .skills-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
    }
    .skill-card-inner {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
    }
    .skill-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
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
    .status {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .grid-full {
      grid-column: 1 / -1;
    }
    @media (max-width: 768px) {
      .skills-grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 480px) {
      .skills-grid {
        grid-template-columns: 1fr;
      }
      .install-row input {
        width: 100%;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
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
      ScToast.show({ message: enable ? "Skill enabled" : "Skill disabled", variant: "success" });
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to update skill";
      ScToast.show({ message: this.error, variant: "error" });
    }
  }

  private async installSkill(): Promise<void> {
    const gw = this.gateway;
    const raw = this.installUrl.trim();
    if (!gw || !raw) return;
    this.error = "";
    const name =
      raw
        .split("/")
        .pop()
        ?.replace(/\.skill\.json$/, "") || raw;
    try {
      await gw.request("skills.install", { name, url: raw });
      this.installUrl = "";
      ScToast.show({ message: "Skill installed", variant: "success" });
      await this.loadSkills();
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Install failed";
      ScToast.show({ message: this.error, variant: "error" });
    }
  }

  private _renderHeader() {
    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Skills"
          description="Installed and available agent skill packages"
        >
          <div class="install-row">
            <sc-input
              type="url"
              placeholder="https://..."
              aria-label="Skill URL to install"
              .value=${this.installUrl}
              @sc-input=${(e: CustomEvent<{ value: string }>) => (this.installUrl = e.detail.value)}
            ></sc-input>
            <sc-button variant="primary" @click=${this.installSkill}>Install</sc-button>
          </div>
        </sc-section-header>
      </sc-page-hero>
    `;
  }

  private _renderSkeleton() {
    return html`
      <div class="skills-grid sc-stagger">
        <sc-skeleton variant="card" height="100px"></sc-skeleton>
        <sc-skeleton variant="card" height="100px"></sc-skeleton>
        <sc-skeleton variant="card" height="100px"></sc-skeleton>
      </div>
    `;
  }

  private _renderSkillGrid() {
    if (this.skills.length === 0) {
      return html`
        <div class="skills-grid sc-stagger">
          <div class="grid-full">
            <sc-empty-state
              .icon=${icons.puzzle}
              heading="No skills installed"
              description="Install skills to extend your agent's capabilities."
            ></sc-empty-state>
          </div>
        </div>
      `;
    }
    return html`
      <div class="skills-grid sc-stagger">
        ${this.skills.map(
          (skill) => html`
            <sc-card glass>
              <div class="skill-card-inner">
                <div class="skill-header">
                  <span class="skill-name">${skill.name}</span>
                  <sc-badge variant="success">Installed</sc-badge>
                </div>
                <div class="skill-desc">${skill.description ?? "No description"}</div>
                <div class="skill-footer">
                  <span class="status">${skill.enabled !== false ? "Enabled" : "Disabled"}</span>
                  <sc-switch
                    .checked=${skill.enabled !== false}
                    @sc-change=${() => this.toggleSkill(skill)}
                    .label=${`Toggle ${skill.name}`}
                  ></sc-switch>
                </div>
              </div>
            </sc-card>
          `,
        )}
      </div>
    `;
  }

  override render() {
    return html`
      ${this._renderHeader()}
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      ${this.loading ? this._renderSkeleton() : this._renderSkillGrid()}
    `;
  }
}
