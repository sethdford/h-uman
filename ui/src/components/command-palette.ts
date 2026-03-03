import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

interface Command {
  action: string;
  id: string;
  label: string;
  section: "navigation" | "actions";
  shortcut?: string;
  icon: string;
}

const NAV_ICON = "→";
const ACTION_ICON = "⚙";

const COMMANDS: Command[] = [
  // Navigation
  {
    action: "navigate",
    id: "overview",
    label: "Overview",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "chat",
    label: "Chat",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "sessions",
    label: "Sessions",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "agents",
    label: "Agents",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "models",
    label: "Models",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "voice",
    label: "Voice",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "tools",
    label: "Tools",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "channels",
    label: "Channels",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "skills",
    label: "Skills",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "cron",
    label: "Cron",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "config",
    label: "Config",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "nodes",
    label: "Nodes",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "usage",
    label: "Usage",
    section: "navigation",
    icon: NAV_ICON,
  },
  {
    action: "navigate",
    id: "logs",
    label: "Logs",
    section: "navigation",
    icon: NAV_ICON,
  },
  // Quick actions
  {
    action: "refresh",
    id: "refresh",
    label: "Refresh current view",
    section: "actions",
    icon: ACTION_ICON,
  },
  {
    action: "toggle-sidebar",
    id: "toggle-sidebar",
    label: "Toggle sidebar",
    section: "actions",
    shortcut: "⌘B",
    icon: ACTION_ICON,
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
      background: rgba(0, 0, 0, 0.5);
      backdrop-filter: blur(4px);
      -webkit-backdrop-filter: blur(4px);
      display: flex;
      align-items: flex-start;
      justify-content: center;
      padding-top: 15vh;
      box-sizing: border-box;
    }

    .backdrop[aria-hidden="true"] {
      display: none;
    }

    .panel {
      background: var(--sc-bg-overlay);
      box-shadow: var(--sc-shadow-lg);
      border-radius: var(--sc-radius-lg);
      max-width: 560px;
      width: 100%;
      margin: 0 var(--sc-space-md);
      box-sizing: border-box;
      animation: sc-scale-in var(--sc-duration-normal) var(--sc-ease-out);
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
      max-height: 360px;
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
      flex-shrink: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
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
      padding: 0.125rem 0.375rem;
      border-radius: var(--sc-radius-sm);
      text-transform: lowercase;
    }

    .shortcut {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-faint);
      font-family: var(--sc-font-mono);
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
