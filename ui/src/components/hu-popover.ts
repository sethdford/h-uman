import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

type PopoverPosition = "top" | "bottom" | "left" | "right";

@customElement("hu-popover")
export class ScPopover extends LitElement {
  @property({ type: Boolean }) open = false;
  @property({ type: String }) position: PopoverPosition = "bottom";
  @property({ type: Boolean }) arrow = true;

  private _nativePopover = typeof HTMLElement.prototype.togglePopover === "function";
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
      z-index: var(--hu-z-popover);
      background: color-mix(in srgb, var(--hu-surface-container-high) 88%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      border: 1px solid var(--hu-glass-border-color);
      border-radius: var(--hu-radius-lg);
      box-shadow: var(--hu-shadow-lg);
      padding: var(--hu-space-md);
      opacity: 0;
      visibility: hidden;
      transform: scale(0.96);
      transition:
        opacity var(--hu-duration-fast) var(--hu-ease-out),
        visibility var(--hu-duration-fast),
        transform var(--hu-duration-normal) var(--hu-spring-out);
    }

    .popover.open {
      opacity: 1;
      visibility: visible;
      transform: scale(1);
      animation: hu-fade-in var(--hu-duration-fast) var(--hu-ease-out);
    }

    /* Native popover enhancement */
    .popover[popover] {
      margin: 0;
      padding: var(--hu-space-md);
      background: color-mix(in srgb, var(--hu-surface-container-high) 88%, transparent);
      backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-standard-blur))
        saturate(var(--hu-glass-standard-saturate));
      border: 1px solid var(--hu-glass-border-color);
      border-radius: var(--hu-radius-lg);
      box-shadow: var(--hu-shadow-lg);
    }
    .popover[popover]:popover-open {
      opacity: 1;
      visibility: visible;
      transform: scale(1);
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
      margin-top: var(--hu-space-xs);
      transform: translateX(-50%) scale(0.96);
    }
    .popover.position-bottom.open {
      transform: translateX(-50%) scale(1);
    }

    .popover.position-top {
      bottom: 100%;
      left: 50%;
      margin-bottom: var(--hu-space-xs);
      transform: translateX(-50%) scale(0.96);
    }
    .popover.position-top.open {
      transform: translateX(-50%) scale(1);
    }

    .popover.position-left {
      right: 100%;
      top: 50%;
      margin-right: var(--hu-space-xs);
      transform: translateY(-50%) scale(0.96);
    }
    .popover.position-left.open {
      transform: translateY(-50%) scale(1);
    }

    .popover.position-right {
      left: 100%;
      top: 50%;
      margin-left: var(--hu-space-xs);
      transform: translateY(-50%) scale(0.96);
    }
    .popover.position-right.open {
      transform: translateY(-50%) scale(1);
    }

    .arrow {
      position: absolute;
      width: 0;
      height: 0;
      border: var(--hu-space-sm) solid transparent;
    }

    .popover.position-bottom .arrow {
      top: calc(-1 * var(--hu-space-sm));
      left: 50%;
      transform: translateX(-50%);
      border-bottom-color: var(--hu-surface-container-high);
      border-top: none;
    }

    .popover.position-top .arrow {
      bottom: calc(-1 * var(--hu-space-sm));
      left: 50%;
      transform: translateX(-50%);
      border-top-color: var(--hu-surface-container-high);
      border-bottom: none;
    }

    .popover.position-left .arrow {
      right: calc(-1 * var(--hu-space-sm));
      top: 50%;
      transform: translateY(-50%);
      border-left-color: var(--hu-surface-container-high);
      border-right: none;
    }

    .popover.position-right .arrow {
      left: calc(-1 * var(--hu-space-sm));
      top: 50%;
      transform: translateY(-50%);
      border-right-color: var(--hu-surface-container-high);
      border-left: none;
    }
  `;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open") && !this._nativePopover) {
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
    this.dispatchEvent(new CustomEvent("hu-close", { bubbles: true, composed: true }));
  }

  private _toggleNative(): void {
    const popoverEl = this.renderRoot.querySelector<HTMLElement>(".popover");
    popoverEl?.togglePopover();
  }

  private _onToggle(e: Event): void {
    const toggleEvent = e as ToggleEvent;
    const isOpen = toggleEvent.newState === "open";
    if (isOpen !== this.open) {
      this.open = isOpen;
      if (!isOpen) {
        this._close();
      }
    }
  }

  override render() {
    const useNative = this._nativePopover;
    const classes = `popover ${this.open ? "open" : ""} position-${this.position}`;
    return html`
      <div
        class="trigger"
        aria-expanded=${this.open}
        aria-haspopup="true"
        @click=${useNative ? this._toggleNative : undefined}
      >
        <slot></slot>
      </div>
      <div
        class=${classes}
        role="dialog"
        popover=${useNative ? "auto" : nothing}
        @toggle=${useNative ? this._onToggle : undefined}
      >
        ${this.arrow ? html`<span class="arrow" aria-hidden="true"></span>` : null}
        <slot name="content"></slot>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-popover": ScPopover;
  }
}
