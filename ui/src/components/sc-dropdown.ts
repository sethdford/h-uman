import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

export type DropdownItem = {
  id: string;
  label: string;
  icon?: string;
  disabled?: boolean;
  divider?: boolean;
};

type DropdownAlign = "start" | "end";

@customElement("sc-dropdown")
export class ScDropdown extends LitElement {
  @property({ type: Boolean }) open = false;
  @property({ type: Array }) items: DropdownItem[] = [];
  @property({ type: String }) align: DropdownAlign = "start";

  @state() private _focusedIndex = -1;
  private _keyHandler = this._onKeyDown.bind(this);
  private _clickOutsideHandler = this._onClickOutside.bind(this);

  static override styles = css`
    :host {
      display: inline-block;
      position: relative;
    }

    .trigger {
      display: contents;
    }

    .menu {
      position: absolute;
      top: 100%;
      margin-top: var(--sc-space-xs);
      min-width: var(--sc-dropdown-min-width);
      background: var(--sc-bg-overlay);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-lg);
      padding: var(--sc-dropdown-padding);
      z-index: var(--sc-z-dropdown);
      opacity: 0;
      visibility: hidden;
      transform: translateY(calc(-1 * var(--sc-space-xs)));
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        visibility var(--sc-duration-fast),
        transform var(--sc-duration-fast) var(--sc-ease-out);
    }

    .menu.open {
      opacity: 1;
      visibility: visible;
      transform: translateY(0);
    }

    @media (prefers-reduced-motion: reduce) {
      .menu {
        transition: none;
      }
    }

    .menu.align-start {
      left: 0;
      right: auto;
    }

    .menu.align-end {
      left: auto;
      right: 0;
    }

    .menu[role="menu"] {
      /* Menu container */
    }

    .item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      width: 100%;
      padding: var(--sc-dropdown-item-padding-y) var(--sc-dropdown-item-padding-x);
      border: none;
      background: transparent;
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      cursor: pointer;
      border-radius: var(--sc-dropdown-item-radius);
      text-align: left;
      transition: background-color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .item:hover:not(:disabled) {
      background: var(--sc-bg-elevated);
    }

    .item:focus {
      outline: none;
    }

    .item:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .item:disabled {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    .item-icon {
      display: flex;
      align-items: center;
      flex-shrink: 0;
      width: 1rem;
      height: 1rem;
      color: var(--sc-text-muted);
    }

    .item-icon svg {
      width: 1rem;
      height: 1rem;
    }

    .divider {
      height: 1px;
      border: none;
      background: var(--sc-border);
      margin: var(--sc-space-xs) 0;
    }
  `;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this._focusedIndex = -1;
        document.addEventListener("keydown", this._keyHandler);
        document.addEventListener("click", this._clickOutsideHandler);
        requestAnimationFrame(() => this._focusFirst());
      } else {
        document.removeEventListener("keydown", this._keyHandler);
        document.removeEventListener("click", this._clickOutsideHandler);
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    document.removeEventListener("keydown", this._keyHandler);
    document.removeEventListener("click", this._clickOutsideHandler);
  }

  private _getNonDividerItems(): DropdownItem[] {
    return this.items.filter((i) => !i.divider);
  }

  private _focusFirst(): void {
    const nonDivider = this._getNonDividerItems();
    const first = nonDivider.findIndex((i) => !i.disabled);
    if (first >= 0) {
      this._focusedIndex = first;
      this._scrollToFocused();
    }
  }

  private _scrollToFocused(): void {
    const menu = this.renderRoot.querySelector<HTMLElement>('[role="menu"]');
    const items = menu?.querySelectorAll('[role="menuitem"]');
    if (items && this._focusedIndex >= 0 && items[this._focusedIndex]) {
      (items[this._focusedIndex] as HTMLElement).focus();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.open) return;
    const nonDivider = this._getNonDividerItems();
    const selectable = nonDivider.filter((i) => !i.disabled);
    if (selectable.length === 0) return;

    if (e.key === "Escape") {
      e.preventDefault();
      this._close();
      return;
    }

    if (e.key === "ArrowDown") {
      e.preventDefault();
      this._focusedIndex = Math.min(this._focusedIndex + 1, nonDivider.length - 1);
      while (this._focusedIndex < nonDivider.length && nonDivider[this._focusedIndex].disabled) {
        this._focusedIndex++;
      }
      if (this._focusedIndex >= nonDivider.length) {
        this._focusedIndex = selectable.length - 1;
      }
      this._scrollToFocused();
      return;
    }

    if (e.key === "ArrowUp") {
      e.preventDefault();
      this._focusedIndex = Math.max(this._focusedIndex - 1, 0);
      while (this._focusedIndex > 0 && nonDivider[this._focusedIndex].disabled) {
        this._focusedIndex--;
      }
      this._scrollToFocused();
      return;
    }

    if (e.key === "Enter" || e.key === " ") {
      const item = nonDivider[this._focusedIndex];
      if (item && !item.disabled) {
        e.preventDefault();
        this._select(item.id);
      }
    }
  }

  private _onClickOutside(e: MouseEvent): void {
    const root = this.shadowRoot;
    if (!root) return;
    const target = e.target as Node;
    if (!root.contains(target) && !this.contains(target)) {
      this._close();
    }
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("sc-close", { bubbles: true, composed: true }));
  }

  private _select(id: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-select", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
    this._close();
  }

  override render() {
    const nonDivider = this._getNonDividerItems();
    let itemIndex = -1;

    return html`
      <div class="trigger">
        <slot></slot>
      </div>
      ${this.open
        ? html`<div class="menu open align-${this.align}" role="menu" aria-expanded=${this.open}>
            ${this.items.map((item) => {
              if (item.divider) {
                return html`<hr class="divider" aria-hidden="true" />`;
              }
              itemIndex++;
              const isFocused = itemIndex === this._focusedIndex;
              const iconEl =
                item.icon && icons[item.icon]
                  ? html`<span class="item-icon">${icons[item.icon]}</span>`
                  : null;
              return html`
                <button
                  type="button"
                  class="item"
                  role="menuitem"
                  ?disabled=${item.disabled}
                  tabindex=${isFocused ? 0 : -1}
                  @click=${() => !item.disabled && this._select(item.id)}
                >
                  ${iconEl}
                  <span>${item.label}</span>
                </button>
              `;
            })}
          </div>`
        : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-dropdown": ScDropdown;
  }
}
