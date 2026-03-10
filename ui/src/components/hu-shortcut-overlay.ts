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

@customElement("hu-shortcut-overlay")
export class ScShortcutOverlay extends LitElement {
  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 10000;
      background: var(--hu-backdrop-overlay);
      backdrop-filter: blur(var(--hu-glass-prominent-blur));
      -webkit-backdrop-filter: blur(var(--hu-glass-prominent-blur));
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-lg);
      box-sizing: border-box;
      animation: hu-fade-in var(--hu-duration-normal) var(--hu-ease-out);
    }

    .backdrop[aria-hidden="true"] {
      display: none;
    }

    .panel {
      background: color-mix(in srgb, var(--hu-bg-overlay) 90%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      box-shadow: var(--hu-shadow-xl);
      border: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 8%),
          transparent
        );
      border-radius: var(--hu-radius-xl);
      max-width: 30rem;
      width: 100%;
      padding: var(--hu-space-xl);
      box-sizing: border-box;
      animation: hu-bounce-in var(--hu-duration-moderate) var(--hu-ease-out);
    }

    .title {
      font-size: var(--hu-text-xl);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-lg);
      font-family: var(--hu-font);
    }

    .category {
      margin-bottom: var(--hu-space-lg);
    }

    .category:last-child {
      margin-bottom: 0;
    }

    .category-title {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: var(--hu-tracking-xs, 0.05em);
      margin: 0 0 var(--hu-space-sm);
      font-family: var(--hu-font);
    }

    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: var(--hu-space-sm) var(--hu-space-xl);
      align-items: baseline;
    }

    .shortcut-cell {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-2xs);
    }

    kbd {
      display: inline-block;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font-mono);
      color: var(--hu-text);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      box-shadow: 0 1px 0 var(--hu-border);
    }

    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
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
    "hu-shortcut-overlay": ScShortcutOverlay;
  }
}
