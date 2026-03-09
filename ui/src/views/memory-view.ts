import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-button.js";
import "../components/sc-input.js";
import "../components/sc-empty-state.js";
import "../components/sc-skeleton.js";
import "../components/sc-stat-card.js";
import "../components/sc-stats-row.js";
import "../components/sc-segmented-control.js";

interface MemoryEntry {
  key: string;
  content: string;
  category: string;
  source?: string;
  timestamp?: string;
}

interface MemoryStatus {
  engine: string;
  total_entries: number;
  categories: Record<string, number>;
}

interface GraphEntity {
  id: number;
  name: string;
  type: string;
  recall_count: number;
  x?: number;
  y?: number;
  vx?: number;
  vy?: number;
}

interface GraphRelation {
  source: number;
  target: number;
  type: string;
  weight: number;
}

const CATEGORY_OPTIONS = [
  { value: "all", label: "All" },
  { value: "core", label: "Core" },
  { value: "daily", label: "Daily" },
  { value: "conversation", label: "Conversation" },
  { value: "insight", label: "Insights" },
];

function categoryVariant(cat: string): "success" | "warning" | "info" | "neutral" {
  switch (cat) {
    case "core":
      return "success";
    case "insight":
      return "info";
    case "daily":
      return "warning";
    default:
      return "neutral";
  }
}

function formatTimestamp(ts?: string): string {
  if (!ts) return "";
  try {
    const d = new Date(ts);
    const diff = Date.now() - d.getTime();
    const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: "auto" });
    if (diff < 60_000) return rtf.format(-Math.floor(diff / 1000), "second");
    if (diff < 3_600_000) return rtf.format(-Math.floor(diff / 60_000), "minute");
    if (diff < 86_400_000) return rtf.format(-Math.floor(diff / 3_600_000), "hour");
    if (diff < 604_800_000) return rtf.format(-Math.floor(diff / 86_400_000), "day");
    return d.toLocaleDateString(undefined, {
      month: "short",
      day: "numeric",
      year: "numeric",
    });
  } catch {
    return ts;
  }
}

function sourceLabel(source?: string): string {
  if (!source) return "";
  if (source.startsWith("file://")) return source.slice(7);
  if (source.startsWith("conversation:")) return source.slice(13);
  if (source === "connection_discovery") return "Auto-insight";
  if (source.startsWith("api-ingest:")) return source.slice(11);
  return source;
}

@customElement("sc-memory-view")
export class ScMemoryView extends GatewayAwareLitElement {
  static override styles = css`
    :host {
      view-transition-name: view-memory;
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
      color: var(--sc-text);
      max-width: 72rem;
    }
    .layout {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-lg);
    }
    .controls {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      flex-wrap: wrap;
    }
    .controls sc-input {
      flex: 1;
      min-width: 12rem;
    }
    .memory-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(20rem, 1fr));
      gap: var(--sc-space-md);
    }
    .memory-card {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }
    .memory-card .key {
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .memory-card .content {
      font-size: var(--sc-text-sm);
      line-height: var(--sc-leading-relaxed);
      color: var(--sc-text);
    }
    .memory-card .meta {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      flex-wrap: wrap;
      margin-top: var(--sc-space-xs);
    }
    .memory-card .source {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
    }
    .memory-card .timestamp {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      margin-left: auto;
    }
    .insight-card {
      border-left: 3px solid var(--sc-accent-tertiary);
    }
    .consolidate-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }
    .consolidate-row .last-run {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .error-banner {
      padding: var(--sc-space-md);
      background: color-mix(in srgb, var(--sc-error) 10%, transparent);
      border-radius: var(--sc-radius);
      color: var(--sc-error);
      font-size: var(--sc-text-sm);
    }
    .skeleton-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(20rem, 1fr));
      gap: var(--sc-space-md);
    }

    @media (max-width: 48rem) /* --sc-breakpoint-md */ {
      .memory-grid {
        grid-template-columns: 1fr;
      }
      .skeleton-grid {
        grid-template-columns: 1fr;
      }
    }
  `;

  @state() private status: MemoryStatus | null = null;
  @state() private entries: MemoryEntry[] = [];
  @state() private loading = true;
  @state() private error = "";
  @state() private searchQuery = "";
  @state() private categoryFilter = "all";
  @state() private consolidating = false;
  @state() private graphEntities: GraphEntity[] = [];
  @state() private graphRelations: GraphRelation[] = [];

  protected override autoRefreshInterval = 30_000;

  protected override async load(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.loading = true;
    this.error = "";
    this.actionError = "";
    try {
      const [statusRes, listRes, graphRes] = await Promise.all([
        gw.request<MemoryStatus>("memory.status"),
        gw.request<{ entries?: MemoryEntry[] }>("memory.list"),
        gw
          .request<{ entities?: GraphEntity[]; relations?: GraphRelation[] }>("memory.graph")
          .catch(() => ({ entities: [], relations: [] })),
      ]);
      this.status = statusRes ?? null;
      this.entries = listRes?.entries ?? [];
      this.graphEntities = graphRes?.entities ?? [];
      this.graphRelations = graphRes?.relations ?? [];
      if (this.graphEntities.length > 0) {
        this._runGraphSimulation();
      }
    } catch (e) {
      this.error = e instanceof Error ? e.message : "Failed to load memory data";
      this.status = null;
      this.entries = [];
      this.graphEntities = [];
      this.graphRelations = [];
    } finally {
      this.loading = false;
    }
  }

  private get filteredEntries(): MemoryEntry[] {
    let result = this.entries;
    if (this.categoryFilter !== "all") {
      result = result.filter((e) => e.category === this.categoryFilter);
    }
    if (this.searchQuery.trim()) {
      const q = this.searchQuery.toLowerCase();
      result = result.filter(
        (e) =>
          e.content.toLowerCase().includes(q) ||
          e.key.toLowerCase().includes(q) ||
          (e.source && e.source.toLowerCase().includes(q)),
      );
    }
    return result;
  }

  @state() private actionError = "";

  private async _consolidate(): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.consolidating = true;
    this.actionError = "";
    try {
      await gw.request("memory.consolidate");
      await this.load();
    } catch (e) {
      this.actionError = e instanceof Error ? e.message : "Consolidation failed";
    } finally {
      this.consolidating = false;
    }
  }

  private async _forget(key: string): Promise<void> {
    const gw = this.gateway;
    if (!gw) return;
    this.actionError = "";
    try {
      await gw.request("memory.forget", { key });
      this.entries = this.entries.filter((e) => e.key !== key);
    } catch (e) {
      this.actionError = e instanceof Error ? e.message : "Failed to forget";
    }
  }

  private _runGraphSimulation(): void {
    const entities = this.graphEntities;
    const relations = this.graphRelations;
    if (entities.length === 0) return;

    const W = 400;
    const H = 400;
    const centerX = W / 2;
    const centerY = H / 2;
    const radius = Math.min(W, H) * 0.35;

    // Circle layout init
    const n = entities.length;
    for (let i = 0; i < n; i++) {
      const angle = (2 * Math.PI * i) / n - Math.PI / 2;
      entities[i].x = centerX + radius * Math.cos(angle);
      entities[i].y = centerY + radius * Math.sin(angle);
      entities[i].vx = 0;
      entities[i].vy = 0;
    }

    const idToIdx = new Map<number, number>();
    entities.forEach((e: GraphEntity, i: number) => idToIdx.set(e.id, i));

    const REPULSE = 800;
    const ATTRACT = 0.02;
    const CENTER = 0.01;
    const DAMP = 0.85;
    const MAX_ITER = 200;
    const STABLE_THRESH = 0.5;

    let iter = 0;
    const step = (): void => {
      let maxDx = 0;
      let maxDy = 0;

      for (let i = 0; i < n; i++) {
        let fx = 0;
        let fy = 0;
        const xi = entities[i].x ?? centerX;
        const yi = entities[i].y ?? centerY;

        // Repulsion from other nodes
        for (let j = 0; j < n; j++) {
          if (i === j) continue;
          const xj = entities[j].x ?? centerX;
          const yj = entities[j].y ?? centerY;
          const dx = xi - xj;
          const dy = yi - yj;
          const d = Math.sqrt(dx * dx + dy * dy) || 0.1;
          const f = REPULSE / (d * d);
          fx += (dx / d) * f;
          fy += (dy / d) * f;
        }

        // Attraction along edges
        for (const r of relations) {
          const si = idToIdx.get(r.source);
          const ti = idToIdx.get(r.target);
          if (si == null || ti == null || si === ti) continue;
          const xs = entities[si].x ?? centerX;
          const ys = entities[si].y ?? centerY;
          const xt = entities[ti].x ?? centerX;
          const yt = entities[ti].y ?? centerY;
          if (i === si) {
            const dx = xt - xi;
            const dy = yt - yi;
            const d = Math.sqrt(dx * dx + dy * dy) || 0.1;
            const f = d * ATTRACT * (r.weight || 0.5);
            fx += (dx / d) * f;
            fy += (dy / d) * f;
          } else if (i === ti) {
            const dx = xs - xi;
            const dy = ys - yi;
            const d = Math.sqrt(dx * dx + dy * dy) || 0.1;
            const f = d * ATTRACT * (r.weight || 0.5);
            fx += (dx / d) * f;
            fy += (dy / d) * f;
          }
        }

        // Centering
        fx += (centerX - xi) * CENTER;
        fy += (centerY - yi) * CENTER;

        const vx = ((entities[i].vx ?? 0) + fx) * DAMP;
        const vy = ((entities[i].vy ?? 0) + fy) * DAMP;
        entities[i].vx = vx;
        entities[i].vy = vy;
        entities[i].x = Math.max(20, Math.min(W - 20, xi + vx));
        entities[i].y = Math.max(20, Math.min(H - 20, yi + vy));
        maxDx = Math.max(maxDx, Math.abs(vx));
        maxDy = Math.max(maxDy, Math.abs(vy));
      }

      iter++;
      this.requestUpdate();

      if (iter < MAX_ITER && (maxDx > STABLE_THRESH || maxDy > STABLE_THRESH)) {
        requestAnimationFrame(step);
      }
    };
    requestAnimationFrame(step);
  }

  override render() {
    return html`
      <sc-page-hero
        heading="Memory"
        description="Browse, search, and manage stored memories and auto-generated insights."
        .icon=${icons.brain}
      ></sc-page-hero>

      <div class="layout">
        ${this.loading
          ? this._renderSkeleton()
          : this.error
            ? this._renderError()
            : this._renderContent()}
      </div>
    `;
  }

  private _renderSkeleton() {
    return html`
      <sc-stats-row>
        ${[1, 2, 3, 4].map(
          () => html`<sc-skeleton variant="card" style="min-height:5rem"></sc-skeleton>`,
        )}
      </sc-stats-row>
      <div class="skeleton-grid">
        ${[1, 2, 3, 4, 5, 6].map(
          () => html`<sc-skeleton variant="card" style="min-height:8rem"></sc-skeleton>`,
        )}
      </div>
    `;
  }

  private _renderError() {
    return html`
      <div class="error-banner" role="alert">
        ${this.error}
        <sc-button size="sm" variant="ghost" @click=${() => this.load()} label="Retry"></sc-button>
      </div>
    `;
  }

  private _renderContent() {
    const s = this.status;
    const filtered = this.filteredEntries;
    const cats = s?.categories ?? {};

    return html`
      <sc-stats-row>
        <sc-stat-card
          label="Total Memories"
          value=${String(s?.total_entries ?? this.entries.length)}
          .icon=${icons.brain}
        ></sc-stat-card>
        <sc-stat-card
          label="Core"
          value=${String(cats.core ?? 0)}
          .icon=${icons["bookmark-simple"]}
        ></sc-stat-card>
        <sc-stat-card
          label="Insights"
          value=${String(cats.insight ?? 0)}
          .icon=${icons.zap}
        ></sc-stat-card>
        <sc-stat-card label="Engine" value=${s?.engine ?? "—"} .icon=${icons.cpu}></sc-stat-card>
      </sc-stats-row>

      <sc-section-header
        heading="Memories"
        description="${filtered.length} ${filtered.length === 1 ? "entry" : "entries"}"
      >
        <div slot="actions" class="consolidate-row">
          <sc-button
            size="sm"
            variant="ghost"
            ?disabled=${this.consolidating}
            @click=${() => this._consolidate()}
            >${this.consolidating ? "Running…" : "Consolidate"}</sc-button
          >
        </div>
      </sc-section-header>

      ${this.actionError
        ? html`<div class="error-banner" role="alert">${this.actionError}</div>`
        : nothing}

      <div class="controls">
        <sc-input
          placeholder="Search memories…"
          .value=${this.searchQuery}
          @input=${(e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          }}
          aria-label="Search memories"
        ></sc-input>
        <sc-segmented-control
          .options=${CATEGORY_OPTIONS}
          .value=${this.categoryFilter}
          @change=${(e: CustomEvent) => {
            this.categoryFilter = e.detail;
          }}
        ></sc-segmented-control>
      </div>

      ${filtered.length === 0
        ? html`<sc-empty-state
            heading="No memories found"
            description=${this.searchQuery || this.categoryFilter !== "all"
              ? "Try adjusting your search or filter."
              : "Memories will appear here as seaclaw learns from conversations and ingested files."}
            .icon=${icons.brain}
          ></sc-empty-state>`
        : html`
            <div class="memory-grid" role="list" aria-label="Memory entries">
              ${filtered.map((entry) => this._renderEntry(entry))}
            </div>
          `}
      ${this.graphEntities.length > 0
        ? html`
            <sc-section-header
              heading="Knowledge Graph"
              description="Entities and relations in the knowledge graph"
            ></sc-section-header>
            <sc-card>
              <div class="graph-container" style="background: var(--sc-surface-container)">
                <svg
                  part="graph"
                  viewBox="0 0 400 400"
                  preserveAspectRatio="xMidYMid meet"
                  aria-label="Knowledge graph visualization"
                >
                  ${this.graphRelations.map((rel: GraphRelation) => {
                    const src = this.graphEntities.find((e: GraphEntity) => e.id === rel.source);
                    const tgt = this.graphEntities.find((e: GraphEntity) => e.id === rel.target);
                    if (
                      !src ||
                      !tgt ||
                      src.x == null ||
                      src.y == null ||
                      tgt.x == null ||
                      tgt.y == null
                    )
                      return nothing;
                    return html`<line
                      x1=${src.x}
                      y1=${src.y}
                      x2=${tgt.x}
                      y2=${tgt.y}
                      stroke="var(--sc-border)"
                      stroke-width="1"
                    />`;
                  })}
                  ${this.graphEntities.map((e: GraphEntity) => {
                    const x = e.x ?? 200;
                    const y = e.y ?? 200;
                    const r = Math.max(10, Math.min(22, 10 + (e.recall_count ?? 0) * 2));
                    return html`
                      <g>
                        <circle
                          cx=${x}
                          cy=${y}
                          r=${r}
                          fill="var(--sc-accent)"
                          aria-label=${e.name}
                        />
                        <text
                          x=${x}
                          y=${y + r + 12}
                          text-anchor="middle"
                          fill="var(--sc-text-primary)"
                          font-size="10"
                          font-family="var(--sc-font)"
                        >
                          ${e.name.length > 14 ? e.name.slice(0, 12) + "…" : e.name}
                        </text>
                      </g>
                    `;
                  })}
                </svg>
              </div>
            </sc-card>
          `
        : nothing}
    `;
  }

  private _renderEntry(entry: MemoryEntry) {
    const isInsight = entry.category === "insight";
    return html`
      <sc-card hoverable class=${isInsight ? "insight-card" : ""} role="listitem">
        <div class="memory-card">
          <div class="key">${entry.key}</div>
          <div class="content">${entry.content}</div>
          <div class="meta">
            <sc-badge variant=${categoryVariant(entry.category)} label=${entry.category}></sc-badge>
            ${entry.source
              ? html`<span class="source">${sourceLabel(entry.source)}</span>`
              : nothing}
            ${entry.timestamp
              ? html`<span class="timestamp">${formatTimestamp(entry.timestamp)}</span>`
              : nothing}
          </div>
        </div>
      </sc-card>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-memory-view": ScMemoryView;
  }
}
