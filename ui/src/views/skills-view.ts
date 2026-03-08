import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import type { InstalledSkill, RegistrySkill } from "../components/sc-skill-card.js";
import type { SelectedSkill } from "../components/sc-skill-detail.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";
import "../components/sc-input.js";
import "../components/sc-stat-card.js";
import "../components/sc-stats-row.js";
import "../components/sc-skill-card.js";
import "../components/sc-skill-registry.js";
import "../components/sc-skill-detail.js";

function parseTags(tags?: string): string[] {
  if (!tags) return [];
  return tags
    .split(",")
    .map((t) => t.trim().toLowerCase())
    .filter(Boolean);
}

@customElement("sc-skills-view")
export class ScSkillsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-skills;
      display: block;
      color: var(--sc-text);
    }
    .toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--sc-space-sm);
      margin-bottom: var(--sc-space-lg);
    }
    .toolbar-left {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      flex: 1;
      min-width: 12.5rem;
      max-width: 22.5rem;
    }
    .toolbar-right {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .staleness {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .section {
      margin-bottom: var(--sc-space-2xl);
    }
    .section-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-md);
    }
    .section-title {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }
    .section-count {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    .tag-chips {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-xs);
      margin-bottom: var(--sc-space-lg);
    }
    .tag-chip {
      display: inline-flex;
      align-items: center;
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      border: 1px solid var(--sc-border);
      background: transparent;
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
      transition: all var(--sc-duration-fast) var(--sc-ease-out);
    }
    .tag-chip:hover {
      color: var(--sc-text);
      border-color: var(--sc-text-muted);
    }
    .tag-chip:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .tag-chip[aria-checked="true"] {
      background: var(--sc-accent);
      color: var(--sc-bg);
      border-color: var(--sc-accent);
    }
    .skills-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(17.5rem, 1fr));
      gap: var(--sc-space-lg);
    }
    .grid-full {
      grid-column: 1 / -1;
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .skills-grid {
        grid-template-columns: 1fr;
      }
    }
    @media (max-width: 48rem) /* --sc-breakpoint-lg */ {
      .skills-grid {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .tag-chip,
      .skill-card,
      :host {
        animation-duration: 0s !important;
        transition-duration: 0s !important;
      }
    }
  `;

  @state() private skills: InstalledSkill[] = [];
  @state() private registryResults: RegistrySkill[] = [];
  @state() private loading = false;
  @state() private registryLoading = false;
  @state() private error = "";
  @state() private localSearch = "";
  @state() private registryQuery = "";
  @state() private selectedSkill: SelectedSkill | null = null;
  @state() private activeTag = "";
  @state() private actionLoading = false;
  @state() private installUrl = "";
  @state() private installUrlError = "";
  private _searchTimer = 0;

  override disconnectedCallback(): void {
    if (this._searchTimer) {
      clearTimeout(this._searchTimer);
      this._searchTimer = 0;
    }
    super.disconnectedCallback();
  }

  private _validateInstallUrl(url: string): string {
    const trimmed = url.trim();
    if (!trimmed) return "";
    try {
      const u = new URL(trimmed);
      if (u.protocol !== "http:" && u.protocol !== "https:") return "URL must use http or https";
      if (!u.hostname) return "Invalid URL";
      return "";
    } catch {
      return "Enter a valid URL";
    }
  }

  private get filteredSkills(): InstalledSkill[] {
    let list = this.skills;
    if (this.localSearch) {
      const q = this.localSearch.toLowerCase();
      list = list.filter(
        (s) => s.name.toLowerCase().includes(q) || (s.description ?? "").toLowerCase().includes(q),
      );
    }
    if (this.activeTag) {
      const tag = this.activeTag.toLowerCase();
      list = list.filter((s) => parseTags(s.tags).includes(tag));
    }
    return list;
  }

  private get statusCounts() {
    const total = this.skills.length;
    const enabled = this.skills.filter((s) => s.enabled).length;
    return { total, enabled, disabled: total - enabled, registry: this.registryResults.length };
  }

  private get allTags(): string[] {
    const tagSet = new Set<string>();
    for (const entry of this.registryResults) for (const t of parseTags(entry.tags)) tagSet.add(t);
    for (const skill of this.skills) for (const t of parseTags(skill.tags)) tagSet.add(t);
    return Array.from(tagSet).sort();
  }

  private get installedNames(): Set<string> {
    return new Set(this.skills.map((s) => s.name));
  }

  private get filteredRegistryResults(): RegistrySkill[] {
    if (!this.activeTag) return this.registryResults;
    const tag = this.activeTag.toLowerCase();
    return this.registryResults.filter((r) => parseTags(r.tags).includes(tag));
  }

  protected override async load(): Promise<void> {
    if (!this.gateway) return;
    this.loading = true;
    this.error = "";
    try {
      await Promise.all([this._fetchInstalled(), this._fetchRegistry("")]);
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load";
    } finally {
      this.loading = false;
    }
  }

  private async _fetchInstalled(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    const res = (await gw.request<{ skills?: InstalledSkill[] }>("skills.list", {})) as
      | { skills?: InstalledSkill[] }
      | { result?: { skills?: InstalledSkill[] } };
    const skills =
      (res && "skills" in res && res.skills) ||
      (res && "result" in res && res.result?.skills) ||
      [];
    this.skills = Array.isArray(skills) ? skills : [];
  }

  private async _fetchRegistry(query: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.registryLoading = true;
    try {
      const res = (await gw.request<{ entries?: RegistrySkill[] }>("skills.search", { query })) as
        | { entries?: RegistrySkill[] }
        | { result?: { entries?: RegistrySkill[] } };
      const entries =
        (res && "entries" in res && res.entries) ||
        (res && "result" in res && res.result?.entries) ||
        [];
      this.registryResults = Array.isArray(entries) ? entries : [];
    } finally {
      this.registryLoading = false;
    }
  }

  private _onRegistrySearch(e: CustomEvent<{ value: string }>) {
    this.registryQuery = e.detail.value;
    if (this._searchTimer) clearTimeout(this._searchTimer);
    this._searchTimer = window.setTimeout(() => {
      this._fetchRegistry(this.registryQuery);
    }, 300);
  }

  private async _toggleSkill(skill: InstalledSkill): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    const enable = !skill.enabled;
    try {
      await gw.request(enable ? "skills.enable" : "skills.disable", { name: skill.name });
      skill.enabled = enable;
      this.requestUpdate();
      ScToast.show({ message: enable ? "Skill enabled" : "Skill disabled", variant: "success" });
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : "Failed to toggle",
        variant: "error",
      });
    }
  }

  private async _installFromRegistry(entry: RegistrySkill): Promise<void> {
    const gw = this.gateway;
    if (!gw || !entry.name) return;
    this.actionLoading = true;
    try {
      await gw.request("skills.install", { name: entry.name, url: entry.url ?? "" });
      ScToast.show({ message: "Installed " + entry.name, variant: "success" });
      await Promise.all([this._fetchInstalled(), this._fetchRegistry(this.registryQuery)]);
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : "Install failed",
        variant: "error",
      });
    } finally {
      this.actionLoading = false;
    }
  }

  private async _installFromUrl(): Promise<void> {
    const gw = this.gateway;
    const raw = this.installUrl.trim();
    const err = this._validateInstallUrl(raw);
    if (err) {
      this.installUrlError = err;
      return;
    }
    if (!gw || !raw) return;
    const name =
      raw
        .split("/")
        .pop()
        ?.replace(/\.skill\.json$/, "") || raw;
    this.actionLoading = true;
    try {
      await gw.request("skills.install", { name, url: raw });
      this.installUrl = "";
      this.installUrlError = "";
      ScToast.show({ message: "Skill installed", variant: "success" });
      await this._fetchInstalled();
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : "Install failed",
        variant: "error",
      });
    } finally {
      this.actionLoading = false;
    }
  }

  private async _uninstallSkill(name: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.actionLoading = true;
    try {
      await gw.request("skills.uninstall", { name });
      ScToast.show({ message: "Uninstalled " + name, variant: "success" });
      this.selectedSkill = null;
      await Promise.all([this._fetchInstalled(), this._fetchRegistry(this.registryQuery)]);
    } catch (e) {
      ScToast.show({
        message: e instanceof Error ? e.message : "Uninstall failed",
        variant: "error",
      });
    } finally {
      this.actionLoading = false;
    }
  }

  private _renderHero(): TemplateResult {
    return html` <sc-page-hero role="region" aria-label="Skills">
      <sc-section-header
        heading="Skills"
        description="Extend your agent with installable skill packages"
      >
        <div class="toolbar-right">
          <sc-input
            type="url"
            placeholder="Install from URL..."
            aria-label="Skill URL to install"
            .value=${this.installUrl}
            .error=${this.installUrlError}
            @sc-input=${(e: CustomEvent<{ value: string }>) => {
              this.installUrl = e.detail.value;
              this.installUrlError = this._validateInstallUrl(e.detail.value);
            }}
          ></sc-input>
          <sc-button
            variant="primary"
            ?disabled=${!this.installUrl.trim() || !!this.installUrlError || this.actionLoading}
            @click=${this._installFromUrl}
            aria-label="Install skill from URL"
            >Install</sc-button
          >
        </div>
      </sc-section-header>
    </sc-page-hero>`;
  }

  private _renderStats(): TemplateResult {
    const c = this.statusCounts;
    return html`<sc-stats-row>
      <sc-stat-card
        label="Installed"
        .value=${c.total}
        accent="primary"
        style="--sc-stagger-delay: 0ms"
      ></sc-stat-card>
      <sc-stat-card
        label="Enabled"
        .value=${c.enabled}
        accent="secondary"
        style="--sc-stagger-delay: 50ms"
      ></sc-stat-card>
      <sc-stat-card
        label="Disabled"
        .value=${c.disabled}
        accent="tertiary"
        style="--sc-stagger-delay: 100ms"
      ></sc-stat-card>
      <sc-stat-card
        label="Registry"
        .value=${c.registry}
        accent="primary"
        style="--sc-stagger-delay: 150ms"
      ></sc-stat-card>
    </sc-stats-row>`;
  }

  private _renderTagChips(): TemplateResult | typeof nothing {
    const tags = this.allTags;
    if (tags.length === 0) return nothing;
    return html`<div class="tag-chips" role="radiogroup" aria-label="Filter by tag">
      <button
        class="tag-chip"
        role="radio"
        aria-checked=${!this.activeTag}
        @click=${() => (this.activeTag = "")}
      >
        All
      </button>
      ${tags.map(
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
    </div>`;
  }

  private _renderInstalledSection(): TemplateResult {
    const filtered = this.filteredSkills;
    return html`<div class="section">
      <div class="section-head">
        <div>
          <span class="section-title">Your Skills</span>
          <span class="section-count">(${this.skills.length})</span>
        </div>
      </div>
      ${this._renderTagChips()}
      <div class="toolbar">
        <div class="toolbar-left">
          <sc-input
            placeholder="Filter installed skills..."
            aria-label="Search installed skills"
            .value=${this.localSearch}
            @sc-input=${(e: CustomEvent<{ value: string }>) => (this.localSearch = e.detail.value)}
          ></sc-input>
        </div>
        <div class="toolbar-right">
          <span class="staleness">${this.stalenessLabel}</span>
          <sc-button variant="ghost" @click=${() => this.load()} aria-label="Refresh skills list"
            >${icons.refresh} Refresh</sc-button
          >
        </div>
      </div>
      ${filtered.length === 0 && this.skills.length > 0
        ? html`<sc-empty-state
            .icon=${icons.magnifyingGlass}
            heading=${this.localSearch ? `No results for "${this.localSearch}"` : "No matches"}
            description="Try a different search or clear filters."
          ></sc-empty-state>`
        : filtered.length === 0
          ? html`<sc-empty-state
              .icon=${icons.puzzle}
              heading="No skills installed"
              description="Install skills from the registry below or paste a URL above."
            ></sc-empty-state>`
          : html`<div class="skills-grid sc-stagger">
              ${filtered.map(
                (s) => html`
                  <sc-skill-card
                    variant="installed"
                    .skill=${s}
                    @skill-select=${() => (this.selectedSkill = { source: "installed", skill: s })}
                    @skill-toggle=${() => this._toggleSkill(s)}
                  ></sc-skill-card>
                `,
              )}
            </div>`}
    </div>`;
  }

  private _renderRegistrySection(): TemplateResult {
    return html`
      <sc-skill-registry
        .results=${this.registryResults}
        .tags=${this.allTags}
        .query=${this.registryQuery}
        .installedNames=${Array.from(this.installedNames)}
        .loading=${this.registryLoading}
        .actionLoading=${this.actionLoading}
        @registry-search=${(e: CustomEvent<{ query: string }>) => {
          this.registryQuery = e.detail.query;
          this._fetchRegistry(e.detail.query);
        }}
        @skill-select=${(e: CustomEvent<{ skill: RegistrySkill }>) =>
          (this.selectedSkill = { source: "registry", skill: e.detail.skill })}
        @install=${(e: CustomEvent<{ skill: RegistrySkill }>) =>
          this._installFromRegistry(e.detail.skill)}
      ></sc-skill-registry>
    `;
  }

  private _renderDetailSheet(): TemplateResult | typeof nothing {
    if (!this.selectedSkill) return nothing;
    return html`
      <sc-skill-detail
        .skill=${this.selectedSkill}
        .installedNames=${Array.from(this.installedNames)}
        .actionLoading=${this.actionLoading}
        @sc-close=${() => (this.selectedSkill = null)}
        @skill-toggle=${(e: CustomEvent<{ skill: InstalledSkill }>) =>
          this._toggleSkill(e.detail.skill)}
        @skill-uninstall=${(e: CustomEvent<{ name: string }>) =>
          this._uninstallSkill(e.detail.name)}
        @install=${(e: CustomEvent<{ skill: RegistrySkill }>) =>
          this._installFromRegistry(e.detail.skill)}
      ></sc-skill-detail>
    `;
  }

  private _renderSkeleton(): TemplateResult {
    return html`<sc-stats-row>
        <sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton>
      </sc-stats-row>
      <div class="skills-grid sc-stagger">
        <sc-skeleton variant="card" height="140px"></sc-skeleton
        ><sc-skeleton variant="card" height="140px"></sc-skeleton
        ><sc-skeleton variant="card" height="140px"></sc-skeleton>
      </div>`;
  }

  override render() {
    return html` ${this._renderHero()}
    ${this.error
      ? html`<sc-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
          <sc-button variant="primary" @click=${() => this.load()} aria-label="Retry loading skills"
            >Retry</sc-button
          >
        </sc-empty-state>`
      : nothing}
    ${this.loading
      ? this._renderSkeleton()
      : html`${this._renderStats()}${this._renderInstalledSection()}${this._renderRegistrySection()}`}
    ${this._renderDetailSheet()}`;
  }
}
