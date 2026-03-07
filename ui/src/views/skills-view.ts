import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import { ScToast } from "../components/sc-toast.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";
import "../components/sc-input.js";
import "../components/sc-stat-card.js";
import "../components/sc-sheet.js";
import "../components/sc-switch.js";
import "../components/sc-json-viewer.js";

interface InstalledSkill {
  name: string;
  description?: string;
  parameters?: string;
  tags?: string;
  enabled: boolean;
}

interface RegistrySkill {
  name: string;
  description?: string;
  version?: string;
  author?: string;
  url?: string;
  tags?: string;
}

type SelectedSkill =
  | { source: "installed"; skill: InstalledSkill }
  | { source: "registry"; skill: RegistrySkill };

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
    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
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
    .skill-card {
      cursor: pointer;
      transition: box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .skill-card:hover {
      box-shadow: var(--sc-shadow-md);
    }
    .skill-card:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .skill-card-inner {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
      min-height: 7.5rem;
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
      font-size: var(--sc-text-base);
    }
    .skill-desc {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
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
      gap: var(--sc-space-sm);
    }
    .skill-tags {
      display: flex;
      gap: var(--sc-space-xs);
      flex-wrap: wrap;
      flex: 1;
      min-width: 0;
    }
    .registry-meta {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .detail-header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    .detail-icon {
      width: 40px;
      height: 40px;
      border-radius: var(--sc-radius-lg);
      background: var(--sc-bg-inset);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--sc-text-muted);
      flex-shrink: 0;
    }
    .detail-icon svg {
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
    }
    .detail-name {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-bold);
      color: var(--sc-text);
    }
    .detail-desc {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      line-height: 1.6;
      margin-bottom: var(--sc-space-xl);
    }
    .detail-grid {
      display: grid;
      grid-template-columns: auto 1fr;
      gap: var(--sc-space-sm) var(--sc-space-xl);
      margin-bottom: var(--sc-space-2xl);
      font-size: var(--sc-text-sm);
    }
    .detail-label {
      color: var(--sc-text-muted);
      font-weight: var(--sc-weight-medium);
    }
    .detail-value {
      color: var(--sc-text);
      font-family: var(--sc-font-mono);
      word-break: break-all;
    }
    .detail-params {
      margin-bottom: var(--sc-space-2xl);
    }
    .detail-params-title {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-sm);
    }
    .detail-params-code {
      background: var(--sc-bg-inset);
      border-radius: var(--sc-radius);
      padding: var(--sc-space-md);
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      overflow-x: auto;
      white-space: pre-wrap;
    }
    .detail-actions {
      display: flex;
      gap: var(--sc-space-sm);
      flex-wrap: wrap;
    }
    .registry-search-row {
      margin-bottom: var(--sc-space-lg);
      max-width: 25rem;
    }
    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .stats-row {
        grid-template-columns: 1fr;
      }
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
    return html` <sc-page-hero>
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
    return html`<div class="stats-row">
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
    </div>`;
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

  private _renderInstalledCard(skill: InstalledSkill): TemplateResult {
    return html`<sc-card
      glass
      class="skill-card"
      tabindex="0"
      role="button"
      aria-label=${`View ${skill.name} details`}
      @click=${() => (this.selectedSkill = { source: "installed", skill })}
      @keydown=${(e: KeyboardEvent) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          this.selectedSkill = { source: "installed", skill };
        }
      }}
    >
      <div class="skill-card-inner">
        <div class="skill-header">
          <span class="skill-name">${skill.name}</span>
          <sc-badge variant=${skill.enabled ? "success" : "neutral"}
            >${skill.enabled ? "Enabled" : "Disabled"}</sc-badge
          >
        </div>
        <div class="skill-desc">${skill.description ?? "No description"}</div>
        <div class="skill-footer">
          <div class="skill-tags"></div>
          <sc-switch
            .checked=${skill.enabled}
            .label=${`Toggle ${skill.name}`}
            @sc-change=${(e: Event) => {
              e.stopPropagation();
              this._toggleSkill(skill);
            }}
          ></sc-switch>
        </div>
      </div>
    </sc-card>`;
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
              ${filtered.map((s) => this._renderInstalledCard(s))}
            </div>`}
    </div>`;
  }

  private _renderRegistryCard(entry: RegistrySkill): TemplateResult {
    const installed = this.installedNames.has(entry.name);
    const tags = parseTags(entry.tags);
    return html`<sc-card
      class="skill-card"
      tabindex="0"
      role="button"
      aria-label=${`View ${entry.name} in registry`}
      @click=${() => (this.selectedSkill = { source: "registry", skill: entry })}
      @keydown=${(e: KeyboardEvent) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          this.selectedSkill = { source: "registry", skill: entry };
        }
      }}
    >
      <div class="skill-card-inner">
        <div class="skill-header">
          <span class="skill-name">${entry.name}</span>
          ${entry.version ? html`<sc-badge variant="info">${entry.version}</sc-badge>` : nothing}
        </div>
        <div class="skill-desc">${entry.description ?? "No description"}</div>
        <div class="registry-meta">
          ${entry.author ? html`<span>by ${entry.author}</span>` : nothing}
        </div>
        <div class="skill-footer">
          <div class="skill-tags">
            ${tags.map((t) => html`<sc-badge variant="neutral">${t}</sc-badge>`)}
          </div>
          ${installed
            ? html`<sc-badge variant="success">Installed</sc-badge>`
            : html`<sc-button
                variant="primary"
                size="sm"
                ?disabled=${this.actionLoading}
                @click=${(e: Event) => {
                  e.stopPropagation();
                  this._installFromRegistry(entry);
                }}
                >Install</sc-button
              >`}
        </div>
      </div>
    </sc-card>`;
  }

  private _renderRegistrySection(): TemplateResult {
    return html`<div class="section">
      <div class="section-head">
        <div>
          <span class="section-title">Explore Registry</span>
          <span class="section-count">(${this.registryResults.length})</span>
        </div>
      </div>
      <div class="registry-search-row">
        <sc-input
          placeholder="Search the skill registry..."
          aria-label="Search skill registry"
          .value=${this.registryQuery}
          @sc-input=${this._onRegistrySearch}
        ></sc-input>
      </div>
      ${this.registryLoading
        ? html`<div class="skills-grid sc-stagger">
            <sc-skeleton variant="card" height="140px"></sc-skeleton
            ><sc-skeleton variant="card" height="140px"></sc-skeleton
            ><sc-skeleton variant="card" height="140px"></sc-skeleton>
          </div>`
        : this.filteredRegistryResults.length === 0
          ? html`<sc-empty-state
              .icon=${icons.compass}
              heading=${this.registryQuery
                ? `No results for "${this.registryQuery}"`
                : this.activeTag
                  ? "No registry skills with this tag"
                  : "No results"}
              description=${this.activeTag
                ? "Try a different tag or search query."
                : "Try a different search query."}
            ></sc-empty-state>`
          : html`<div class="skills-grid sc-stagger">
              ${this.filteredRegistryResults.map((e) => this._renderRegistryCard(e))}
            </div>`}
    </div>`;
  }

  private _parseParametersJson(raw: string): unknown {
    try {
      const trimmed = raw.trim();
      if (!trimmed) return null;
      return JSON.parse(trimmed);
    } catch {
      return null;
    }
  }

  private _renderParametersViewer(params: string): TemplateResult {
    const parsed = this._parseParametersJson(params);
    if (parsed !== null && typeof parsed === "object") {
      return html`<sc-json-viewer .data=${parsed} root-label="Parameters"></sc-json-viewer>`;
    }
    return html`<div class="detail-params-code">${params}</div>`;
  }

  private _renderDetailSheet(): TemplateResult | typeof nothing {
    const sel = this.selectedSkill;
    if (!sel) return nothing;
    const isInstalled = sel.source === "installed";
    const skill = sel.skill;
    const name = skill.name;
    const desc = skill.description ?? "No description";
    return html`<sc-sheet .open=${true} size="md" @sc-close=${() => (this.selectedSkill = null)}>
      <div class="detail-header">
        <div class="detail-icon">${icons.puzzle}</div>
        <div>
          <div class="detail-name">${name}</div>
          <sc-badge variant=${isInstalled ? "success" : "info"}
            >${isInstalled ? "Installed" : "Registry"}</sc-badge
          >
        </div>
      </div>
      <div class="detail-desc">${desc}</div>
      <div class="detail-grid">
        ${isInstalled
          ? html`<span class="detail-label">Status</span
              ><span class="detail-value"
                >${(sel.skill as InstalledSkill).enabled ? "Enabled" : "Disabled"}</span
              >`
          : nothing}
        ${"version" in skill && (skill as RegistrySkill).version
          ? html`<span class="detail-label">Version</span
              ><span class="detail-value">${(skill as RegistrySkill).version}</span>`
          : nothing}
        ${"author" in skill && (skill as RegistrySkill).author
          ? html`<span class="detail-label">Author</span
              ><span class="detail-value">${(skill as RegistrySkill).author}</span>`
          : nothing}
        ${"tags" in skill && (skill as RegistrySkill).tags
          ? html`<span class="detail-label">Tags</span
              ><span class="detail-value">${(skill as RegistrySkill).tags}</span>`
          : nothing}
        ${"url" in skill && (skill as RegistrySkill).url
          ? html`<span class="detail-label">URL</span
              ><span class="detail-value">${(skill as RegistrySkill).url}</span>`
          : nothing}
      </div>
      ${isInstalled && (sel.skill as InstalledSkill).parameters
        ? html`<div class="detail-params">
            <div class="detail-params-title">Parameters</div>
            ${this._renderParametersViewer((sel.skill as InstalledSkill).parameters!)}
          </div>`
        : nothing}
      <div class="detail-actions">
        ${isInstalled
          ? html`
              <sc-button
                variant=${(sel.skill as InstalledSkill).enabled ? "secondary" : "primary"}
                ?loading=${this.actionLoading}
                @click=${() => this._toggleSkill(sel.skill as InstalledSkill)}
                >${(sel.skill as InstalledSkill).enabled ? "Disable" : "Enable"}</sc-button
              >
              <sc-button
                variant="destructive"
                ?loading=${this.actionLoading}
                @click=${() => this._uninstallSkill(name)}
                >Uninstall</sc-button
              >
            `
          : html`<sc-button
              variant="primary"
              ?loading=${this.actionLoading}
              ?disabled=${this.installedNames.has(name)}
              @click=${() => this._installFromRegistry(sel.skill as RegistrySkill)}
              >${this.installedNames.has(name) ? "Already Installed" : "Install"}</sc-button
            >`}
      </div>
    </sc-sheet>`;
  }

  private _renderSkeleton(): TemplateResult {
    return html`<div class="stats-row">
        <sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton
        ><sc-skeleton variant="stat-card"></sc-skeleton>
      </div>
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
