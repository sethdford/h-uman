import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

type PopoverPosition = "top" | "bottom" | "left" | "right";

@customElement("sc-popover")
export class ScPopover extends LitElement {
  @property({ type: Boolean }) open = false;
  @property({ type: String }) position: PopoverPosition = "bottom";
  @property({ type: Boolean }) arrow = true;

  private _clickOutsideHandler = this._onClickOutside.bind(this);
  private _keyHandler = this._onKeyDown.bind(this);

  static override styles = css`
    :host {
      display: inline-block;
      position: relative;
    }

    .trigger {
      display: contents;
    }

    .popover {
      position: absolute;
      z-index: var(--sc-z-popover);
      background: color-mix(in srgb, var(--sc-bg-overlay) 88%, transparent);
      backdrop-filter: blur(20px) saturate(180%);
      -webkit-backdrop-filter: blur(20px) saturate(180%);
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-lg);
      padding: var(--sc-space-md);
      opacity: 0;
      visibility: hidden;
      transform: scale(0.96);
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        visibility var(--sc-duration-fast),
        transform var(--sc-duration-normal) var(--sc-spring-out);
    }

    .popover.open {
      opacity: 1;
      visibility: visible;
      transform: scale(1);
      animation: sc-fade-in var(--sc-duration-fast) var(--sc-ease-out);
    }

    @media (prefers-reduced-motion: reduce) {
      .popover {
        transition: none;
      }
      .popover.open {
        animation: none;
      }
    }

    .popover.position-bottom {
      top: 100%;
      left: 50%;
      margin-top: var(--sc-space-xs);
      transform: translateX(-50%) scale(0.96);
    }
    .popover.position-bottom.open {
      transform: translateX(-50%) scale(1);
    }

    .popover.position-top {
      bottom: 100%;
      left: 50%;
      margin-bottom: var(--sc-space-xs);
      transform: translateX(-50%) scale(0.96);
    }
    .popover.position-top.open {
      transform: translateX(-50%) scale(1);
    }

    .popover.position-left {
      right: 100%;
      top: 50%;
      margin-right: var(--sc-space-xs);
      transform: translateY(-50%) scale(0.96);
    }
    .popover.position-left.open {
      transform: translateY(-50%) scale(1);
    }

    .popover.position-right {
      left: 100%;
      top: 50%;
      margin-left: var(--sc-space-xs);
      transform: translateY(-50%) scale(0.96);
    }
    .popover.position-right.open {
      transform: translateY(-50%) scale(1);
    }

    .arrow {
      position: absolute;
      width: 0;
      height: 0;
      border: var(--sc-space-sm) solid transparent;
    }

    .popover.position-bottom .arrow {
      top: calc(-1 * var(--sc-space-sm));
      left: 50%;
      transform: translateX(-50%);
      border-bottom-color: var(--sc-bg-overlay);
      border-top: none;
    }

    .popover.position-top .arrow {
      bottom: calc(-1 * var(--sc-space-sm));
      left: 50%;
      transform: translateX(-50%);
      border-top-color: var(--sc-bg-overlay);
      border-bottom: none;
    }

    .popover.position-left .arrow {
      right: calc(-1 * var(--sc-space-sm));
      top: 50%;
      transform: translateY(-50%);
      border-left-color: var(--sc-bg-overlay);
      border-right: none;
    }

    .popover.position-right .arrow {
      left: calc(-1 * var(--sc-space-sm));
      top: 50%;
      transform: translateY(-50%);
      border-right-color: var(--sc-bg-overlay);
      border-left: none;
    }
  `;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        document.addEventListener("click", this._clickOutsideHandler);
        document.addEventListener("keydown", this._keyHandler);
      } else {
        document.removeEventListener("click", this._clickOutsideHandler);
        document.removeEventListener("keydown", this._keyHandler);
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    document.removeEventListener("click", this._clickOutsideHandler);
    document.removeEventListener("keydown", this._keyHandler);
  }

  private _onClickOutside(e: MouseEvent): void {
    const target = e.target as Node;
    const root = this.shadowRoot;
    if (!root?.contains(target) && !this.contains(target)) {
      this._close();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      e.preventDefault();
      this._close();
      const trigger = this.renderRoot.querySelector<HTMLElement>(".trigger");
      trigger?.focus();
      return;
    }
    if (e.key === "Tab" && this.open) {
      const popover = this.renderRoot.querySelector<HTMLElement>(".popover");
      if (!popover) return;
      const slotEl = popover.querySelector<HTMLSlotElement>("slot[name=content]");
      const assigned = slotEl ? slotEl.assignedElements({ flatten: true }) : [];
      const selectors = 'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
      const focusable = assigned.flatMap((el) => [
        ...(el instanceof HTMLElement && el.matches(selectors) ? [el] : []),
        ...Array.from(el.querySelectorAll<HTMLElement>(selectors)),
      ]);
      if (focusable.length === 0) return;
      const last = focusable[focusable.length - 1];
      const first = focusable[0];
      if (!e.shiftKey && document.activeElement === last) {
        e.preventDefault();
        first.focus();
      } else if (e.shiftKey && document.activeElement === first) {
        e.preventDefault();
        last.focus();
      }
    }
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("sc-close", { bubbles: true, composed: true }));
  }

  override render() {
    return html`
      <div class="trigger" aria-expanded=${this.open} aria-haspopup="true">
        <slot></slot>
      </div>
      ${this.open
        ? html`
            <div class="popover open position-${this.position}" role="dialog">
              ${this.arrow ? html`<span class="arrow" aria-hidden="true"></span>` : null}
              <slot name="content"></slot>
            </div>
          `
        : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-popover": ScPopover;
  }
}
