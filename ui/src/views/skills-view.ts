import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/hu-toast.js";
import type { InstalledSkill, RegistrySkill } from "../components/hu-skill-card.js";
import type { SelectedSkill } from "../components/hu-skill-detail.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-card.js";
import "../components/hu-empty-state.js";
import "../components/hu-skeleton.js";
import "../components/hu-input.js";
import "../components/hu-stat-card.js";
import "../components/hu-stats-row.js";
import "../components/hu-skill-card.js";
import "../components/hu-skill-registry.js";
import "../components/hu-skill-detail.js";

function friendlyError(e: unknown): string {
  const msg = e instanceof Error ? e.message : String(e);
  if (msg.includes("timeout")) return "Request timed out. Please try again.";
  if (msg.includes("WebSocket")) return "Connection lost. Reconnecting...";
  if (msg.includes("404")) return "Resource not found.";
  if (msg.includes("401") || msg.includes("unauthorized"))
    return "Authentication failed. Please check your credentials.";
  if (msg.includes("403") || msg.includes("forbidden")) return "Access denied.";
  if (msg.includes("network")) return "Network error. Please check your connection.";
  return "Something went wrong. Please try again.";
}

function parseTags(tags?: string): string[] {
  if (!tags) return [];
  return tags
    .split(",")
    .map((t) => t.trim().toLowerCase())
    .filter(Boolean);
}

@customElement("hu-skills-view")
export class ScSkillsView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;

  static override styles = css`
    :host {
      view-transition-name: view-skills;
      display: block;
      color: var(--hu-text);
    }
    .toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: var(--hu-space-sm);
      margin-bottom: var(--hu-space-lg);
    }
    .toolbar-left {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      flex: 1;
      min-width: 12.5rem;
      max-width: 22.5rem;
    }
    .toolbar-right {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }
    .staleness {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
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
    .grid-full {
      grid-column: 1 / -1;
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
      this.error = friendlyError(e);
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
    return html` <hu-page-hero role="region" aria-label="Skills">
      <hu-section-header
        heading="Skills"
        description="Extend your agent with installable skill packages"
      >
        <div class="toolbar-right">
          <hu-input
            type="url"
            placeholder="Install from URL..."
            aria-label="Skill URL to install"
            .value=${this.installUrl}
            .error=${this.installUrlError}
            @hu-input=${(e: CustomEvent<{ value: string }>) => {
              this.installUrl = e.detail.value;
              this.installUrlError = this._validateInstallUrl(e.detail.value);
            }}
          ></hu-input>
          <hu-button
            variant="primary"
            ?disabled=${!this.installUrl.trim() || !!this.installUrlError || this.actionLoading}
            @click=${this._installFromUrl}
            aria-label="Install skill from URL"
            >Install</hu-button
          >
        </div>
      </hu-section-header>
    </hu-page-hero>`;
  }

  private _renderStats(): TemplateResult {
    const c = this.statusCounts;
    return html`<hu-stats-row>
      <hu-stat-card
        label="Installed"
        .value=${c.total}
        accent="primary"
        style="--hu-stagger-delay: 0ms"
      ></hu-stat-card>
      <hu-stat-card
        label="Enabled"
        .value=${c.enabled}
        accent="secondary"
        style="--hu-stagger-delay: 50ms"
      ></hu-stat-card>
      <hu-stat-card
        label="Disabled"
        .value=${c.disabled}
        accent="tertiary"
        style="--hu-stagger-delay: 100ms"
      ></hu-stat-card>
      <hu-stat-card
        label="Registry"
        .value=${c.registry}
        accent="primary"
        style="--hu-stagger-delay: 150ms"
      ></hu-stat-card>
    </hu-stats-row>`;
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
          <hu-input
            placeholder="Filter installed skills..."
            aria-label="Search installed skills"
            .value=${this.localSearch}
            @hu-input=${(e: CustomEvent<{ value: string }>) => (this.localSearch = e.detail.value)}
          ></hu-input>
        </div>
        <div class="toolbar-right">
          <span class="staleness">${this.stalenessLabel}</span>
          <hu-button variant="ghost" @click=${() => this.load()} aria-label="Refresh skills list"
            >${icons.refresh} Refresh</hu-button
          >
        </div>
      </div>
      ${filtered.length === 0 && this.skills.length > 0
        ? html`<hu-empty-state
            .icon=${icons.magnifyingGlass}
            heading=${this.localSearch ? `No results for "${this.localSearch}"` : "No matches"}
            description="Try a different search or clear filters."
          ></hu-empty-state>`
        : filtered.length === 0
          ? html`<hu-empty-state
              .icon=${icons.puzzle}
              heading="No skills installed"
              description="Install skills from the registry below or paste a URL above."
            ></hu-empty-state>`
          : html`<div class="skills-grid hu-stagger">
              ${filtered.map(
                (s) => html`
                  <hu-skill-card
                    variant="installed"
                    .skill=${s}
                    @skill-select=${() => (this.selectedSkill = { source: "installed", skill: s })}
                    @skill-toggle=${() => this._toggleSkill(s)}
                  ></hu-skill-card>
                `,
              )}
            </div>`}
    </div>`;
  }

  private _renderRegistrySection(): TemplateResult {
    return html`
      <hu-skill-registry
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
      ></hu-skill-registry>
    `;
  }

  private _renderDetailSheet(): TemplateResult | typeof nothing {
    if (!this.selectedSkill) return nothing;
    return html`
      <hu-skill-detail
        .skill=${this.selectedSkill}
        .installedNames=${Array.from(this.installedNames)}
        .actionLoading=${this.actionLoading}
        @hu-close=${() => (this.selectedSkill = null)}
        @skill-toggle=${(e: CustomEvent<{ skill: InstalledSkill }>) =>
          this._toggleSkill(e.detail.skill)}
        @skill-uninstall=${(e: CustomEvent<{ name: string }>) =>
          this._uninstallSkill(e.detail.name)}
        @install=${(e: CustomEvent<{ skill: RegistrySkill }>) =>
          this._installFromRegistry(e.detail.skill)}
      ></hu-skill-detail>
    `;
  }

  private _renderSkeleton(): TemplateResult {
    return html`<hu-stats-row>
        <hu-skeleton variant="stat-card"></hu-skeleton
        ><hu-skeleton variant="stat-card"></hu-skeleton
        ><hu-skeleton variant="stat-card"></hu-skeleton
        ><hu-skeleton variant="stat-card"></hu-skeleton>
      </hu-stats-row>
      <div class="skills-grid hu-stagger">
        <hu-skeleton variant="card" height="140px"></hu-skeleton
        ><hu-skeleton variant="card" height="140px"></hu-skeleton
        ><hu-skeleton variant="card" height="140px"></hu-skeleton>
      </div>`;
  }

  override render() {
    return html` ${this._renderHero()}
    ${this.error
      ? html`<hu-empty-state .icon=${icons.warning} heading="Error" description=${this.error}>
          <hu-button variant="primary" @click=${() => this.load()} aria-label="Retry loading skills"
            >Retry</hu-button
          >
        </hu-empty-state>`
      : nothing}
    ${this.loading
      ? this._renderSkeleton()
      : html`${this._renderStats()}${this._renderInstalledSection()}${this._renderRegistrySection()}`}
    ${this._renderDetailSheet()}`;
  }
}

