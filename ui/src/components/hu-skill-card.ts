import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-switch.js";

export interface InstalledSkill {
  name: string;
  description?: string;
  parameters?: string;
  tags?: string;
  enabled: boolean;
}

export interface RegistrySkill {
  name: string;
  description?: string;
  version?: string;
  author?: string;
  url?: string;
  tags?: string;
}

function parseTags(tags?: string): string[] {
  if (!tags) return [];
  return tags
    .split(",")
    .map((t) => t.trim().toLowerCase())
    .filter(Boolean);
}

@customElement("hu-skill-card")
export class ScSkillCard extends LitElement {
  @property({ type: String }) variant: "installed" | "registry" = "installed";
  @property({ attribute: false }) skill: InstalledSkill | RegistrySkill = {
    name: "",
    description: "",
    enabled: false,
  };
  @property({ type: Boolean }) installed = false;
  @property({ type: Boolean }) actionLoading = false;

  static override styles = css`
    :host {
      display: block;
    }

    .skill-card {
      cursor: pointer;
      transition: box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .skill-card:hover {
      box-shadow: var(--hu-shadow-md);
    }

    .skill-card:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .skill-card-inner {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
      min-height: 7.5rem;
    }

    .skill-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-sm);
    }

    .skill-name {
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      font-size: var(--hu-text-base);
    }

    .skill-desc {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      flex: 1;
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
    }

    .skill-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-sm);
    }

    .skill-tags {
      display: flex;
      gap: var(--hu-space-xs);
      flex-wrap: wrap;
      flex: 1;
      min-width: 0;
    }

    .registry-meta {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    @media (prefers-reduced-motion: reduce) {
      .skill-card {
        transition: none;
      }
    }
  `;

  private _onCardClick(): void {
    this.dispatchEvent(
      new CustomEvent("skill-select", {
        detail: { skill: this.skill },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onToggle(e: Event): void {
    e.stopPropagation();
    if (this.variant === "installed") {
      this.dispatchEvent(
        new CustomEvent("skill-toggle", {
          detail: { skill: this.skill as InstalledSkill },
          bubbles: true,
          composed: true,
        }),
      );
    }
  }

  private _onInstall(e: Event): void {
    e.stopPropagation();
    if (this.variant === "registry") {
      this.dispatchEvent(
        new CustomEvent("install", {
          detail: { skill: this.skill as RegistrySkill },
          bubbles: true,
          composed: true,
        }),
      );
    }
  }

  override render() {
    const isInstalled = this.variant === "installed";
    const inst = this.skill as InstalledSkill;
    const reg = this.skill as RegistrySkill;

    const ariaLabel =
      this.variant === "installed"
        ? `View ${this.skill.name} details`
        : `View ${this.skill.name} in registry`;

    return html`
      <hu-card
        glass=${isInstalled}
        class="skill-card"
        tabindex="0"
        role="button"
        aria-label=${ariaLabel}
        @click=${this._onCardClick}
        @keydown=${(e: KeyboardEvent) => {
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            this._onCardClick();
          }
        }}
      >
        <div class="skill-card-inner">
          <div class="skill-header">
            <span class="skill-name">${this.skill.name}</span>
            ${isInstalled
              ? html`<hu-badge variant=${inst.enabled ? "success" : "neutral"}
                  >${inst.enabled ? "Enabled" : "Disabled"}</hu-badge
                >`
              : reg.version
                ? html`<hu-badge variant="info">${reg.version}</hu-badge>`
                : nothing}
          </div>
          <div class="skill-desc">${this.skill.description ?? "No description"}</div>
          ${!isInstalled && reg.author
            ? html`<div class="registry-meta"><span>by ${reg.author}</span></div>`
            : nothing}
          <div class="skill-footer">
            <div class="skill-tags">
              ${this.variant === "registry"
                ? parseTags(reg.tags).map((t) => html`<hu-badge variant="neutral">${t}</hu-badge>`)
                : nothing}
            </div>
            ${isInstalled
              ? html`<hu-switch
                  .checked=${inst.enabled}
                  .label=${`Toggle ${this.skill.name}`}
                  @hu-change=${this._onToggle}
                ></hu-switch>`
              : this.installed
                ? html`<hu-badge variant="success">Installed</hu-badge>`
                : html`<hu-button
                    variant="primary"
                    size="sm"
                    ?disabled=${this.actionLoading}
                    @click=${this._onInstall}
                    >Install</hu-button
                  >`}
          </div>
        </div>
      </hu-card>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-skill-card": ScSkillCard;
  }
}
