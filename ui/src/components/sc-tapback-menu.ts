import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

/** Viewport-safety offsets for popover placement (px required for JS coordinates). */
const MENU_WIDTH = 220;
const MENU_OFFSET_Y = 50;
const MENU_MIN_Y = 10;

const REACTIONS = [
  { icon: "thumbs-up", label: "Like", value: "like" },
  { icon: "thumbs-down", label: "Dislike", value: "dislike" },
  { icon: "heart", label: "Heart", value: "heart" },
  { icon: "copy", label: "Copy", value: "copy" },
  { icon: "bookmark-simple", label: "Bookmark", value: "bookmark" },
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
      0% {
        opacity: 0;
        transform: scale(0.8);
      }
      50% {
        transform: scale(1.05);
      }
      100% {
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
      animation: sc-tapback-enter var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
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
      background: var(--sc-hover-overlay);
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
    if (reaction.value === "copy") navigator.clipboard?.writeText(this.messageContent);
    this.dispatchEvent(
      new CustomEvent("sc-react", {
        bubbles: true,
        composed: true,
        detail: { value: reaction.value, index: this.messageIndex },
      }),
    );
  }

  private _onOverlayClick() {
    this.dispatchEvent(new CustomEvent("sc-tapback-close", { bubbles: true, composed: true }));
  }

  private _onKeydown = (e: KeyboardEvent) => {
    if (e.key === "Escape") {
      this.dispatchEvent(new CustomEvent("sc-tapback-close", { bubbles: true, composed: true }));
      return;
    }
    if (!this.open) return;
    const bar = this.shadowRoot?.querySelector(".bar");
    if (!bar) return;
    const items = Array.from(bar.querySelectorAll<HTMLElement>(".reaction-btn"));
    if (items.length === 0) return;
    const active = this.shadowRoot?.activeElement as HTMLElement | null;
    const idx = active ? items.indexOf(active) : -1;

    if (e.key === "ArrowRight" || e.key === "ArrowDown") {
      e.preventDefault();
      items[(idx + 1) % items.length].focus();
    } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
      e.preventDefault();
      items[(idx - 1 + items.length) % items.length].focus();
    } else if (e.key === "Home") {
      e.preventDefault();
      items[0].focus();
    } else if (e.key === "End") {
      e.preventDefault();
      items[items.length - 1].focus();
    }
  };

  override connectedCallback() {
    super.connectedCallback();
    document.addEventListener("keydown", this._onKeydown);
  }
  override disconnectedCallback() {
    document.removeEventListener("keydown", this._onKeydown);
    super.disconnectedCallback();
  }
  override updated(changed: Map<PropertyKey, unknown>) {
    if (changed.has("open") && this.open) {
      requestAnimationFrame(() => {
        const first = this.shadowRoot?.querySelector<HTMLElement>(".reaction-btn");
        first?.focus();
      });
    }
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
