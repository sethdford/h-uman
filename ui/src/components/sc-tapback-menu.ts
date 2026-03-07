import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

/** Viewport-safety offsets for popover placement (px required for JS coordinates). */
const MENU_WIDTH = 220;
const MENU_OFFSET_Y = 50;
const MENU_MIN_Y = 10;

const REACTIONS = [
  { key: "👍", icon: "thumbs-up", label: "Thumbs up" }, // sc-lint-ok
  { key: "👎", icon: "thumbs-down", label: "Thumbs down" }, // sc-lint-ok
  { key: "❤️", icon: "heart", label: "Heart" }, // sc-lint-ok
  { key: "📋", icon: "copy", label: "Copy" }, // sc-lint-ok
  { key: "🔖", icon: "bookmark-simple", label: "Bookmark" }, // sc-lint-ok
] as const;

@customElement("sc-tapback-menu")
export class ScTapbackMenu extends LitElement {
  @property({ type: Boolean, reflect: true }) open = false;
  @property({ type: Number }) x = 0;
  @property({ type: Number }) y = 0;
  @property({ type: Number }) messageIndex = -1;
  @property({ type: String }) messageContent = "";

  static override styles = css`
    @keyframes sc-tapback-enter {
      from {
        opacity: 0;
        transform: scale(0.8);
      }
      to {
        opacity: 1;
        transform: scale(1);
      }
    }
    :host {
      display: block;
    }
    :host(:not([open])) .overlay {
      display: none;
    }
    .overlay {
      position: fixed;
      inset: 0;
      z-index: 100;
    }
    .bar {
      position: fixed;
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      padding: var(--sc-space-xs);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-full);
      box-shadow: var(--sc-shadow-lg);
      z-index: 101;
      animation: sc-tapback-enter var(--sc-duration-fast) var(--sc-spring-micro);
    }
    .reaction-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-space-2xl);
      height: var(--sc-space-2xl);
      border-radius: var(--sc-radius-full);
      border: none;
      background: transparent;
      cursor: pointer;
      color: var(--sc-text);
      font-size: var(--sc-text-lg);
      transition: background var(--sc-duration-fast);
    }
    .reaction-btn:hover {
      background: var(--sc-bg-elevated);
    }
    .reaction-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .reaction-btn svg {
      width: var(--sc-icon-lg);
      height: var(--sc-icon-lg);
    }
    @media (prefers-reduced-motion: reduce) {
      .bar {
        animation: none;
      }
    }
  `;

  private _onClick(reaction: (typeof REACTIONS)[number]) {
    if (reaction.icon === "copy") navigator.clipboard?.writeText(this.messageContent);
    this.dispatchEvent(
      new CustomEvent("sc-react", {
        bubbles: true,
        composed: true,
        detail: { emoji: reaction.key, index: this.messageIndex },
      }),
    );
  }

  private _onOverlayClick() {
    this.dispatchEvent(new CustomEvent("sc-tapback-close", { bubbles: true, composed: true }));
  }

  private _onKeydown = (e: KeyboardEvent) => {
    if (e.key === "Escape")
      this.dispatchEvent(new CustomEvent("sc-tapback-close", { bubbles: true, composed: true }));
  };

  override connectedCallback() {
    super.connectedCallback();
    document.addEventListener("keydown", this._onKeydown);
  }
  override disconnectedCallback() {
    document.removeEventListener("keydown", this._onKeydown);
    super.disconnectedCallback();
  }

  override render() {
    if (!this.open) return nothing;
    const barX = Math.min(this.x, window.innerWidth - MENU_WIDTH);
    const barY = Math.max(this.y - MENU_OFFSET_Y, MENU_MIN_Y);
    return html`
      <div class="overlay" @click=${this._onOverlayClick}></div>
      <div
        class="bar"
        role="menu"
        aria-label="Message reactions"
        style="left:${barX}px;top:${barY}px;"
      >
        ${REACTIONS.map(
          (r) => html`
            <button
              class="reaction-btn"
              role="menuitem"
              aria-label=${r.label}
              @click=${(e: Event) => {
                e.stopPropagation();
                this._onClick(r);
              }}
            >
              ${icons[r.icon]}
            </button>
          `,
        )}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-tapback-menu": ScTapbackMenu;
  }
}
