import { LitElement, html, css, nothing, type TemplateResult } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

interface Command {
  action: string;
  id: string;
  label: string;
  section: "navigation" | "actions";
  shortcut?: string;
  icon: TemplateResult;
}

const NAV_ICON_MAP: Record<string, TemplateResult> = {
  overview: icons.grid,
  chat: icons["message-square"],
  sessions: icons.clock,
  agents: icons.zap,
  models: icons.cpu,
  voice: icons.mic,
  tools: icons.wrench,
  channels: icons.radio,
  skills: icons.puzzle,
  cron: icons.timer,
  config: icons.settings,
  security: icons.shield,
  nodes: icons.server,
  usage: icons["bar-chart"],
  logs: icons["file-text"],
};

const NAV_ITEMS: { id: string; label: string }[] = [
  { id: "overview", label: "Overview" },
  { id: "chat", label: "Chat" },
  { id: "sessions", label: "Sessions" },
  { id: "agents", label: "Agents" },
  { id: "models", label: "Models" },
  { id: "voice", label: "Voice" },
  { id: "tools", label: "Tools" },
  { id: "channels", label: "Channels" },
  { id: "skills", label: "Skills" },
  { id: "cron", label: "Cron" },
  { id: "config", label: "Config" },
  { id: "security", label: "Security" },
  { id: "nodes", label: "Nodes" },
  { id: "usage", label: "Usage" },
  { id: "logs", label: "Logs" },
];

const COMMANDS: Command[] = [
  ...NAV_ITEMS.map((n) => ({
    action: "navigate",
    id: n.id,
    label: n.label,
    section: "navigation" as const,
    icon: NAV_ICON_MAP[n.id] ?? icons["arrow-right"],
  })),
  {
    action: "refresh",
    id: "refresh",
    label: "Refresh current view",
    section: "actions",
    icon: icons.refresh,
  },
  {
    action: "toggle-sidebar",
    id: "toggle-sidebar",
    label: "Toggle sidebar",
    section: "actions",
    shortcut: "\u2318B",
    icon: icons["sidebar-toggle"],
  },
];

function filterCommands(query: string): Command[] {
  const q = query.toLowerCase().trim();
  if (!q) return COMMANDS;
  return COMMANDS.filter((c) => c.label.toLowerCase().includes(q));
}

function highlightMatch(label: string, query: string): string | ReturnType<typeof html> {
  const q = query.toLowerCase().trim();
  if (!q || !label.toLowerCase().includes(q)) {
    return label;
  }
  const lower = label.toLowerCase();
  const idx = lower.indexOf(q);
  const before = label.slice(0, idx);
  const match = label.slice(idx, idx + q.length);
  const after = label.slice(idx + q.length);
  return html`${before}<strong>${match}</strong>${after}`;
}

@customElement("sc-command-palette")
export class ScCommandPalette extends LitElement {
  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 10000;
      background: var(--sc-backdrop-overlay);
      backdrop-filter: blur(var(--sc-blur-md, 12px));
      -webkit-backdrop-filter: blur(var(--sc-blur-md, 12px));
      display: flex;
      align-items: flex-start;
      justify-content: center;
      padding-top: 15%;
      box-sizing: border-box;
    }

    .backdrop[aria-hidden="true"] {
      display: none;
    }

    .panel {
      background: color-mix(in srgb, var(--sc-bg-overlay) 85%, transparent);
      backdrop-filter: blur(24px) saturate(180%);
      -webkit-backdrop-filter: blur(24px) saturate(180%);
      box-shadow: var(--sc-shadow-xl);
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: var(--sc-radius-xl);
      max-width: 560px;
      width: 100%;
      margin: 0 var(--sc-space-md);
      box-sizing: border-box;
      animation: sc-bounce-in var(--sc-duration-moderate) var(--sc-ease-out);
    }

    .input-wrap {
      padding: var(--sc-space-md);
      border-bottom: 1px solid var(--sc-border);
    }

    .input {
      width: 100%;
      padding: var(--sc-space-md);
      font-size: var(--sc-text-lg);
      font-family: var(--sc-font);
      color: var(--sc-text);
      background: var(--sc-bg-overlay);
      border: none;
      outline: none;
    }

    .input::placeholder {
      color: var(--sc-text-muted);
    }

    .results {
      max-height: min(24rem, 70vh);
      overflow-y: auto;
      padding: var(--sc-space-xs);
    }

    .item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-md);
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-base);
      color: var(--sc-text);
      cursor: pointer;
      border-radius: var(--sc-radius);
    }

    .item:hover {
      background: var(--sc-bg-elevated);
    }

    .item.selected {
      background: var(--sc-bg-elevated);
    }

    .icon {
      width: 1.25rem;
      height: 1.25rem;
      flex-shrink: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--sc-text-muted);
    }

    .icon svg {
      width: 100%;
      height: 100%;
    }

    .label {
      flex: 1;
      min-width: 0;
    }

    .label strong {
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }

    .meta {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      flex-shrink: 0;
    }

    .badge {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      background: var(--sc-bg-elevated);
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      border-radius: var(--sc-radius-sm);
      text-transform: lowercase;
    }

    .shortcut {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      font-family: var(--sc-font-mono);
    }
    @media (prefers-reduced-motion: reduce) {
      .panel,
      .backdrop {
        animation: none !important;
      }
    }
  `;

  @property({ type: Boolean }) open = false;

  @state() private query = "";
  @state() private selectedIndex = 0;

  private get filteredCommands(): Command[] {
    return filterCommands(this.query);
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this.query = "";
        this.selectedIndex = 0;
        requestAnimationFrame(() => this._focusInput());
      }
    }
    if (changedProperties.has("query")) {
      this.selectedIndex = 0;
    }
  }

  private _focusInput(): void {
    const input = this.renderRoot.querySelector<HTMLInputElement>(".input");
    input?.focus();
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  private _execute(cmd: Command): void {
    this.dispatchEvent(
      new CustomEvent("execute", {
        detail: { action: cmd.action, id: cmd.id },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    const items = this.filteredCommands;
    if (items.length === 0) return;

    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        this.selectedIndex = (this.selectedIndex + 1) % items.length;
        break;
      case "ArrowUp":
        e.preventDefault();
        this.selectedIndex = (this.selectedIndex - 1 + items.length) % items.length;
        break;
      case "Enter":
        e.preventDefault();
        this._execute(items[this.selectedIndex]);
        break;
      case "Escape":
        e.preventDefault();
        this._close();
        break;
      default:
        break;
    }
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  override render() {
    if (!this.open) return nothing;

    const items = this.filteredCommands;
    const sectionLabel = (s: Command["section"]) => (s === "navigation" ? "Navigate" : "Actions");

    return html`
      <div
        class="backdrop"
        role="dialog"
        aria-modal="true"
        aria-hidden=${!this.open}
        @click=${this._onBackdropClick}
      >
        <div class="panel">
          <div class="input-wrap">
            <input
              class="input"
              type="text"
              placeholder="Search commands..."
              .value=${this.query}
              @input=${(e: Event) => {
                this.query = (e.target as HTMLInputElement).value;
              }}
              @keydown=${this._onKeyDown}
            />
          </div>
          <div class="results">
            ${items.length === 0
              ? html`<div class="item" style="color: var(--sc-text-muted)">No results</div>`
              : items.map(
                  (cmd, i) => html`
                    <div
                      class="item ${i === this.selectedIndex ? "selected" : ""}"
                      role="option"
                      aria-selected=${i === this.selectedIndex}
                      tabindex=${i === this.selectedIndex ? 0 : -1}
                      @click=${() => this._execute(cmd)}
                      @mouseenter=${() => (this.selectedIndex = i)}
                    >
                      <span class="icon">${cmd.icon}</span>
                      <span class="label">${highlightMatch(cmd.label, this.query)}</span>
                      <div class="meta">
                        <span class="badge">${sectionLabel(cmd.section)}</span>
                        ${cmd.shortcut
                          ? html`<span class="shortcut">${cmd.shortcut}</span>`
                          : nothing}
                      </div>
                    </div>
                  `,
                )}
          </div>
        </div>
      </div>
    `;
  }
}
