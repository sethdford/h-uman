import { LitElement, html, css, nothing, type TemplateResult } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export type ContextMenuItem =
  | {
      label: string;
      icon?: TemplateResult;
      action: () => void;
      divider?: false;
      disabled?: boolean;
    }
  | { divider: true };

@customElement("hu-context-menu")
export class ScContextMenu extends LitElement {
  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 9999;
      background: transparent;
    }

    .menu {
      position: fixed;
      z-index: 10000;
      min-width: 11.25rem;
      background: color-mix(in srgb, var(--hu-surface-container-highest) 92%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      border: 1px solid
        color-mix(
          in srgb,
          var(--hu-border) var(--hu-glass-standard-border-opacity, 8%),
          transparent
        );
      border-radius: var(--hu-radius-lg);
      box-shadow: var(--hu-shadow-lg);
      padding: var(--hu-space-xs);
      animation: hu-fade-in var(--hu-duration-fast) var(--hu-ease-out);
    }

    .item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      width: 100%;
      padding: var(--hu-space-sm) var(--hu-space-md);
      border: none;
      background: transparent;
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      cursor: pointer;
      border-radius: var(--hu-radius);
      text-align: left;
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .item:hover:not(:disabled),
    .item:focus-visible:not(:disabled),
    .item.focused:not(:disabled) {
      background: var(--hu-hover-overlay);
    }

    .item:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    .item .icon {
      width: 1rem;
      height: 1rem;
      flex-shrink: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--hu-text-muted);
    }

    .item .icon svg {
      width: 100%;
      height: 100%;
    }

    .divider {
      height: 1px;
      background: var(--hu-border);
      margin: var(--hu-space-xs) 0;
    }

    @media (prefers-reduced-motion: reduce) {
      .menu {
        animation: none !important;
      }
    }
  `;

  @property({ type: Boolean }) open = false;
  @property({ type: Number }) x = 0;
  @property({ type: Number }) y = 0;
  @property({ type: Array }) items: ContextMenuItem[] = [];

  @state() private _focusedIndex = 0;

  private _clickOutsideHandler = (e: MouseEvent) => this._onClickOutside(e);
  private _keyHandler = (e: KeyboardEvent) => this._onKeyDown(e);

  private get _actionItems(): Extract<ContextMenuItem, { action: () => void }>[] {
    return this.items.filter(
      (i): i is Extract<ContextMenuItem, { action: () => void }> => !("divider" in i && i.divider),
    );
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this._focusedIndex = 0;
        document.addEventListener("click", this._clickOutsideHandler, true);
        document.addEventListener("keydown", this._keyHandler, true);
        requestAnimationFrame(() => this._focusItem(0));
      } else {
        document.removeEventListener("click", this._clickOutsideHandler, true);
        document.removeEventListener("keydown", this._keyHandler, true);
      }
    }
  }

  override disconnectedCallback(): void {
    document.removeEventListener("click", this._clickOutsideHandler, true);
    document.removeEventListener("keydown", this._keyHandler, true);
    super.disconnectedCallback();
  }

  private _onClickOutside(e: MouseEvent): void {
    const menu = this.renderRoot.querySelector(".menu");
    if (menu && !menu.contains(e.target as Node)) {
      this._close();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this._close();
      return;
    }
    const actionItems = this._actionItems;
    if (actionItems.length === 0) return;

    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        this._focusedIndex = (this._focusedIndex + 1) % actionItems.length;
        this._focusItem(this._focusedIndex);
        break;
      case "ArrowUp":
        e.preventDefault();
        this._focusedIndex = (this._focusedIndex - 1 + actionItems.length) % actionItems.length;
        this._focusItem(this._focusedIndex);
        break;
      case "Enter":
      case " ":
        e.preventDefault();
        const item = actionItems[this._focusedIndex];
        if (item && !item.disabled) {
          item.action();
          this._close();
        }
        break;
      default:
        break;
    }
  }

  private _focusItem(index: number): void {
    const buttons = this.renderRoot.querySelectorAll<HTMLButtonElement>(".item:not([disabled])");
    const btn = buttons[index];
    btn?.focus();
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  private _runAction(item: Extract<ContextMenuItem, { action: () => void }>): void {
    if (item.disabled) return;
    item.action();
    this._close();
  }

  override render() {
    if (!this.open) return nothing;

    let actionIdx = 0;
    return html`
      <div class="backdrop" @click=${this._close} role="none" aria-hidden="true"></div>
      <div
        class="menu"
        role="menu"
        aria-label="Context menu"
        style="left: ${this.x}px; top: ${this.y}px"
      >
        ${this.items.map((item) => {
          if ("divider" in item && item.divider) {
            return html`<div class="divider" role="separator"></div>`;
          }
          const idx = actionIdx++;
          const focused = idx === this._focusedIndex;
          const actionItem = item as Extract<ContextMenuItem, { action: () => void }>;
          return html`
            <button
              class="item ${focused ? "focused" : ""}"
              role="menuitem"
              ?disabled=${actionItem.disabled}
              @click=${() => this._runAction(actionItem)}
              @mouseenter=${() => (this._focusedIndex = idx)}
            >
              ${actionItem.icon ? html`<span class="icon">${actionItem.icon}</span>` : nothing}
              <span>${actionItem.label}</span>
            </button>
          `;
        })}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-context-menu": ScContextMenu;
  }
}
