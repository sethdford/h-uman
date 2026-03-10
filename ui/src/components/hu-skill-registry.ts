import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import type { RegistrySkill } from "./hu-skill-card.js";
import "../components/hu-input.js";
import "../components/hu-empty-state.js";
import "../components/hu-skeleton.js";
import "./hu-skill-card.js";

function parseTags(tags?: string): string[] {
  if (!tags) return [];
  return tags
    .split(",")
    .map((t) => t.trim().toLowerCase())
    .filter(Boolean);
}

@customElement("hu-skill-registry")
export class ScSkillRegistry extends LitElement {
  @property({ type: Array }) results: RegistrySkill[] = [];
  @property({ type: Array }) tags: string[] = [];
  @property({ type: Array }) installedNames: string[] = [];
  @property({ type: Boolean }) loading = false;
  @property({ type: Boolean }) actionLoading = false;
  @property({ type: String }) query = "";
  @state() private activeTag = "";
  private _searchTimer = 0;

  override disconnectedCallback(): void {
    if (this._searchTimer) {
      clearTimeout(this._searchTimer);
      this._searchTimer = 0;
    }
    super.disconnectedCallback();
  }

  private get _filteredResults(): RegistrySkill[] {
    if (!this.activeTag) return this.results;
    const tag = this.activeTag.toLowerCase();
    return this.results.filter((r) => parseTags(r.tags).includes(tag));
  }

  private _onSearch(e: CustomEvent<{ value: string }>): void {
    const value = e.detail.value;
    if (this._searchTimer) clearTimeout(this._searchTimer);
    this._searchTimer = window.setTimeout(() => {
      this.dispatchEvent(
        new CustomEvent("registry-search", {
          detail: { query: value },
          bubbles: true,
          composed: true,
        }),
      );
    }, 300);
  }

  override render() {
    const installedSet = new Set(this.installedNames);

    return html`
      <div class="section">
        <div class="section-head">
          <div>
            <span class="section-title">Explore Registry</span>
            <span class="section-count">(${this.results.length})</span>
          </div>
        </div>
        <div class="registry-search-row">
          <hu-input
            placeholder="Search the skill registry..."
            aria-label="Search skill registry"
            .value=${this.query}
            @hu-input=${this._onSearch}
          ></hu-input>
        </div>
        ${this.tags.length > 0
          ? html`<div class="tag-chips" role="radiogroup" aria-label="Filter by tag">
              <button
                class="tag-chip"
                role="radio"
                aria-checked=${!this.activeTag}
                @click=${() => (this.activeTag = "")}
              >
                All
              </button>
              ${this.tags.map(
                (tag) =>
                  html`<button
                    class="tag-chip"
                    role="radio"
                    aria-checked=${this.activeTag === tag}
                    @click=${() => (this.activeTag = this.activeTag === tag ? "" : tag)}
                  >
                    ${tag}
                  </button>`,
              )}
            </div>`
          : nothing}
        ${this.loading
          ? html`<div class="skills-grid hu-stagger">
              <hu-skeleton variant="card" height="140px"></hu-skeleton>
              <hu-skeleton variant="card" height="140px"></hu-skeleton>
              <hu-skeleton variant="card" height="140px"></hu-skeleton>
            </div>`
          : this._filteredResults.length === 0
            ? html`<hu-empty-state
                .icon=${icons.compass}
                heading=${this.query
                  ? `No results for "${this.query}"`
                  : this.activeTag
                    ? "No registry skills with this tag"
                    : "No results"}
                description=${this.activeTag
                  ? "Try a different tag or search query."
                  : "Try a different search query."}
              ></hu-empty-state>`
            : html`<div class="skills-grid hu-stagger">
                ${this._filteredResults.map(
                  (e) => html`
                    <hu-skill-card
                      variant="registry"
                      .skill=${e}
                      .installed=${installedSet.has(e.name)}
                      .actionLoading=${this.actionLoading}
                      @install=${(ev: CustomEvent<{ skill: RegistrySkill }>) => {
                        this.dispatchEvent(
                          new CustomEvent("install", {
                            detail: ev.detail,
                            bubbles: true,
                            composed: true,
                          }),
                        );
                      }}
                    ></hu-skill-card>
                  `,
                )}
              </div>`}
      </div>
    `;
  }

  static override styles = css`
    :host {
      display: block;
    }

    .section {
      margin-bottom: var(--hu-space-2xl);
    }

    .section-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-md);
      margin-bottom: var(--hu-space-md);
    }

    .section-title {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .section-count {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .registry-search-row {
      margin-bottom: var(--hu-space-lg);
      max-width: 25rem;
    }

    .tag-chips {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      margin-bottom: var(--hu-space-lg);
    }

    .tag-chip {
      display: inline-flex;
      align-items: center;
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      cursor: pointer;
      border: 1px solid var(--hu-border);
      background: transparent;
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
      transition: all var(--hu-duration-fast) var(--hu-ease-out);
    }

    .tag-chip:hover {
      color: var(--hu-text);
      border-color: var(--hu-text-muted);
    }

    .tag-chip:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .tag-chip[aria-checked="true"] {
      background: var(--hu-accent);
      color: var(--hu-bg);
      border-color: var(--hu-accent);
    }

    .skills-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(17.5rem, 1fr));
      gap: var(--hu-space-lg);
    }

    @media (max-width: 30rem) /* --hu-breakpoint-sm */ {
      .skills-grid {
        grid-template-columns: 1fr;
      }
    }

    @media (max-width: 48rem) /* --hu-breakpoint-lg */ {
      .skills-grid {
        grid-template-columns: 1fr 1fr;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .tag-chip {
        transition: none;
      }
    }
  `;
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-skill-registry": ScSkillRegistry;
  }
}
