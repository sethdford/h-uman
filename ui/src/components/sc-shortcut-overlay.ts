import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

interface ShortcutRow {
  shortcut: string;
  description: string;
}

interface ShortcutCategory {
  title: string;
  rows: ShortcutRow[];
}

const SHORTCUTS: ShortcutCategory[] = [
  {
    title: "Navigation",
    rows: [
      { shortcut: "Cmd+K / Ctrl+K", description: "Command palette" },
      { shortcut: "Cmd+B / Ctrl+B", description: "Toggle sidebar" },
      { shortcut: "?", description: "Keyboard shortcuts" },
    ],
  },
  {
    title: "Chat",
    rows: [
      { shortcut: "Enter", description: "Send message" },
      { shortcut: "Shift+Enter", description: "New line" },
      { shortcut: "Escape", description: "Cancel/close" },
    ],
  },
  {
    title: "General",
    rows: [
      { shortcut: "Tab", description: "Navigate focus" },
      { shortcut: "Space/Enter", description: "Activate button" },
      { shortcut: "Arrow keys", description: "Navigate lists" },
    ],
  },
];

@customElement("sc-shortcut-overlay")
export class ScShortcutOverlay extends LitElement {
  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 10000;
      background: var(--sc-backdrop-overlay);
      backdrop-filter: blur(var(--sc-glass-prominent-blur));
      -webkit-backdrop-filter: blur(var(--sc-glass-prominent-blur));
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-lg);
      box-sizing: border-box;
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .backdrop[aria-hidden="true"] {
      display: none;
    }

    .panel {
      background: color-mix(in srgb, var(--sc-bg-overlay) 90%, transparent);
      backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur))
        saturate(var(--sc-glass-standard-saturate));
      box-shadow: var(--sc-shadow-xl);
      border: 1px solid
        color-mix(
          in srgb,
          var(--sc-border) var(--sc-glass-standard-border-opacity, 8%),
          transparent
        );
      border-radius: var(--sc-radius-xl);
      max-width: 480px;
      width: 100%;
      padding: var(--sc-space-xl);
      box-sizing: border-box;
      animation: sc-bounce-in var(--sc-duration-moderate) var(--sc-ease-out);
    }

    .title {
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: 0 0 var(--sc-space-lg);
      font-family: var(--sc-font);
    }

    .category {
      margin-bottom: var(--sc-space-lg);
    }

    .category:last-child {
      margin-bottom: 0;
    }

    .category-title {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text-muted);
      text-transform: uppercase;
      letter-spacing: var(--sc-tracking-xs, 0.05em);
      margin: 0 0 var(--sc-space-sm);
      font-family: var(--sc-font);
    }

    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--sc-space-sm) var(--sc-space-xl);
      align-items: baseline;
    }

    .shortcut-cell {
      display: flex;
      flex-wrap: wrap;
      gap: var(--sc-space-2xs);
    }

    kbd {
      display: inline-block;
      padding: var(--sc-space-2xs) var(--sc-space-xs);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font-mono);
      color: var(--sc-text);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-sm);
      box-shadow: 0 1px 0 var(--sc-border);
    }

    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }

    @media (prefers-reduced-motion: reduce) {
      .backdrop,
      .panel {
        animation: none !important;
      }
    }
  `;

  @property({ type: Boolean }) open = false;

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this._close();
    }
    if (e.key === "Tab") {
      const focusable = this._getFocusable();
      if (focusable.length === 0) {
        e.preventDefault();
        return;
      }
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      const active = document.activeElement;
      if (e.shiftKey) {
        if (active === first) {
          e.preventDefault();
          last.focus();
        }
      } else {
        if (active === last) {
          e.preventDefault();
          first.focus();
        }
      }
    }
  }

  private _getFocusable(): HTMLElement[] {
    const panel = this.renderRoot.querySelector<HTMLElement>(".panel");
    if (!panel) return [];
    const selectors = 'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
    return Array.from(panel.querySelectorAll<HTMLElement>(selectors)).filter(
      (el) => !el.hasAttribute("disabled") && el.offsetParent !== null,
    );
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open") && this.open) {
      requestAnimationFrame(() => {
        const panel = this.renderRoot.querySelector<HTMLElement>(".panel");
        panel?.setAttribute("tabindex", "0");
        panel?.focus();
      });
    }
  }

  override render() {
    if (!this.open) return nothing;

    return html`
      <div
        class="backdrop"
        role="dialog"
        aria-modal="true"
        aria-label="Keyboard shortcuts"
        aria-hidden=${!this.open}
        tabindex="-1"
        @click=${this._onBackdropClick}
        @keydown=${this._onKeyDown}
      >
        <div class="panel" tabindex="0" @click=${(e: MouseEvent) => e.stopPropagation()}>
          <h2 class="title" id="shortcut-overlay-title">Keyboard Shortcuts</h2>
          ${SHORTCUTS.map(
            (cat) => html`
              <div class="category">
                <h3 class="category-title">${cat.title}</h3>
                <div class="grid">
                  ${cat.rows.map(
                    (row) => html`
                      <div class="shortcut-cell">
                        ${row.shortcut.split(" / ").map((part) => html`<kbd>${part}</kbd>`)}
                      </div>
                      <div class="description">${row.description}</div>
                    `,
                  )}
                </div>
              </div>
            `,
          )}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-shortcut-overlay": ScShortcutOverlay;
  }
}
