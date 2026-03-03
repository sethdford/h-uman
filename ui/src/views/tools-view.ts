import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import "../components/sc-card.js";
import "../components/sc-skeleton.js";
import "../components/sc-empty-state.js";

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
    }
    .search {
      margin-bottom: var(--sc-space-md);
      max-width: 400px;
    }
    .search input {
      width: 100%;
      padding: 0.5rem 1rem;
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: 0.875rem;
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
      gap: var(--sc-space-md);
    }
    .grid-full {
      grid-column: 1 / -1;
    }
    .card-name {
      font-weight: var(--sc-weight-semibold);
      font-size: var(--sc-text-lg);
      color: var(--sc-accent);
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
    .schema-toggle {
      background: none;
      border: none;
      color: var(--sc-accent);
      cursor: pointer;
      font-size: var(--sc-text-xs);
      padding: var(--sc-space-xs) 0;
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

  override render() {
    const filtered = this.filteredTools;

    if (this.loading) {
      return html`
        <div class="search">
          <input type="text" placeholder="Search tools..." disabled style="opacity: 0.6" />
        </div>
        <div class="grid">
          <sc-skeleton variant="card" height="80px"></sc-skeleton>
          <sc-skeleton variant="card" height="80px"></sc-skeleton>
          <sc-skeleton variant="card" height="80px"></sc-skeleton>
          <sc-skeleton variant="card" height="80px"></sc-skeleton>
        </div>
      `;
    }

    return html`
      ${this.error
        ? html`<sc-empty-state
            icon="⚠️"
            heading="Error"
            description=${this.error}
          ></sc-empty-state>`
        : nothing}
      <div class="search">
        <input
          type="text"
          placeholder="Search tools..."
          .value=${this.filter}
          @input=${(e: Event) => {
            this.filter = (e.target as HTMLInputElement).value;
          }}
        />
      </div>
      <div class="grid">
        ${filtered.length === 0
          ? html`
              <div class="grid-full">
                <sc-empty-state
                  icon="🔧"
                  heading="No tools available"
                  description="Tools will appear here when the gateway provides them."
                ></sc-empty-state>
              </div>
            `
          : filtered.map(
              (t) => html`
                <sc-card>
                  <div class="card-name">${t.name ?? "unnamed"}</div>
                  <div class="card-desc">${t.description ?? ""}</div>
                  ${t.parameters != null
                    ? html`
                        <button
                          class="schema-toggle"
                          @click=${() => this.toggleSchema(t.name ?? "")}
                        >
                          ${this.expandedCards.has(t.name ?? "") ? "Hide params" : "Show params"}
                        </button>
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
}
