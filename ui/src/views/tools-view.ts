import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-button.js";
import "../components/sc-card.js";
import "../components/sc-input.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";
import "../components/sc-stat-card.js";

interface ToolDef {
  name?: string;
  description?: string;
  parameters?: unknown;
}

@customElement("sc-tools-view")
export class ScToolsView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      display: block;
      max-width: 1200px;
    }
    .stats-row {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-xl);
    }
    .search {
      margin-bottom: var(--sc-space-xl);
      max-width: 400px;
    }
    .search input {
      width: 100%;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-sm);
    }
    .search input:focus {
      outline: none;
      border-color: var(--sc-accent);
    }
    .search input::placeholder {
      color: var(--sc-text-muted);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: var(--sc-space-xl);
    }
    .grid-full {
      grid-column: 1 / -1;
    }
    .card-name {
      font-weight: var(--sc-weight-semibold);
      font-size: var(--sc-text-lg);
      color: var(--sc-accent-text, var(--sc-accent));
      margin-bottom: var(--sc-space-sm);
    }
    .card-desc {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-sm);
      line-height: var(--sc-leading-normal);
    }
    .card-schema {
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font-mono);
      color: var(--sc-text-muted);
      background: var(--sc-bg);
      padding: var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      max-height: 6rem;
      overflow: auto;
    }
    @media (max-width: 640px) {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
    @media (max-width: 768px) {
      .grid {
        grid-template-columns: 1fr 1fr;
      }
      .search {
        max-width: 100%;
      }
    }
    @media (max-width: 480px) {
      .grid {
        grid-template-columns: 1fr;
      }
    }
    @media (prefers-reduced-motion: reduce) {
      * {
        animation-duration: 0s !important;
      }
    }
  `;

  @state() private tools: ToolDef[] = [];
  @state() private loading = true;
  @state() private filter = "";
  @state() private error = "";
  @state() private expandedCards = new Set<string>();

  protected override async load(): Promise<void> {
    await this.loadTools();
  }

  private async loadTools(): Promise<void> {
    if (!this.gateway) {
      this.loading = false;
      return;
    }
    this.loading = true;
    try {
      const payload = await this.gateway.request<{ tools?: ToolDef[] }>("tools.catalog", {});
      this.tools = payload?.tools ?? [];
    } catch (e) {
      this.tools = [];
      this.error = e instanceof Error ? e.message : "Failed to load tools";
    } finally {
      this.loading = false;
    }
  }

  private toggleSchema(name: string): void {
    const next = new Set(this.expandedCards);
    if (next.has(name)) next.delete(name);
    else next.add(name);
    this.expandedCards = next;
  }

  private get filteredTools(): ToolDef[] {
    const q = this.filter.toLowerCase().trim();
    if (!q) return this.tools;
    return this.tools.filter(
      (t) =>
        (t.name ?? "").toLowerCase().includes(q) || (t.description ?? "").toLowerCase().includes(q),
    );
  }

  private _renderSkeleton() {
    return html`
      <div class="search">
        <sc-input type="text" placeholder="Search tools..." disabled></sc-input>
      </div>
      <div class="grid sc-stagger">
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
        <sc-skeleton variant="card" height="80px"></sc-skeleton>
      </div>
    `;
  }

  private _renderContent() {
    const filtered = this.filteredTools;
    const totalTools = this.tools.length;
    const enabledTools = this.tools.length;
    return html`
      <sc-page-hero>
        <sc-section-header
          heading="Tools"
          description="Available capabilities and tool integrations"
        ></sc-section-header>
      </sc-page-hero>
      <div class="stats-row">
        <sc-stat-card
          .value=${totalTools}
          label="Total Tools"
          style="--sc-stagger-delay: 0ms"
        ></sc-stat-card>
        <sc-stat-card
          .value=${enabledTools}
          label="Enabled"
          style="--sc-stagger-delay: 80ms"
        ></sc-stat-card>
      </div>
      ${this.error
        ? html`<sc-empty-state
            .icon=${icons.warning}
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      <div class="search" role="search" aria-label="Filter tools">
        <sc-input
          type="text"
          placeholder="Search tools..."
          aria-label="Search tools"
          .value=${this.filter}
          @sc-input=${(e: CustomEvent<{ value: string }>) => {
            this.filter = e.detail.value;
          }}
        ></sc-input>
      </div>
      <div class="grid sc-stagger">
        ${filtered.length === 0
          ? html`
              <div class="grid-full">
                <sc-empty-state
                  .icon=${icons.wrench}
                  heading="No tools available"
                  description="Tools will appear here when the gateway provides them."
                ></sc-empty-state>
              </div>
            `
          : filtered.map(
              (t) => html`
                <sc-card glass>
                  <div class="card-name">${t.name ?? "unnamed"}</div>
                  <div class="card-desc">${t.description ?? ""}</div>
                  ${t.parameters != null
                    ? html`
                        <sc-button
                          variant="ghost"
                          size="sm"
                          @click=${() => this.toggleSchema(t.name ?? "")}
                          aria-expanded=${this.expandedCards.has(t.name ?? "")}
                          aria-label=${`${this.expandedCards.has(t.name ?? "") ? "Hide" : "Show"} parameters for ${t.name ?? "tool"}`}
                        >
                          ${this.expandedCards.has(t.name ?? "") ? "Hide params" : "Show params"}
                        </sc-button>
                        ${this.expandedCards.has(t.name ?? "")
                          ? html`
                              <pre class="card-schema">
${JSON.stringify(t.parameters, null, 2)}</pre
                              >
                            `
                          : ""}
                      `
                    : ""}
                </sc-card>
              `,
            )}
      </div>
    `;
  }

  override render() {
    if (this.loading) {
      return this._renderSkeleton();
    }
    return this._renderContent();
  }
}
