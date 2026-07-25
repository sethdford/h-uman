import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-modal")
export class ScModal extends LitElement {
  static override styles = css`
    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 9999;
      background: color-mix(in srgb, var(--hu-bg) 50%, transparent);
      backdrop-filter: blur(var(--hu-blur-sm, var(--hu-space-xs)));
      -webkit-backdrop-filter: blur(var(--hu-blur-sm, var(--hu-space-xs)));
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-lg);
      box-sizing: border-box;
      animation: hu-fade-in var(--hu-duration-normal) var(--hu-ease-out);
    }

    .backdrop.closing {
      animation: hu-fade-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    .panel {
      max-width: 30rem;
      width: 100%;
      background: color-mix(in srgb, var(--hu-surface-container-highest) 85%, transparent);
      backdrop-filter: blur(var(--hu-glass-prominent-blur))
        saturate(var(--hu-glass-prominent-saturate));
      -webkit-backdrop-filter: blur(var(--hu-glass-prominent-blur))
        saturate(var(--hu-glass-prominent-saturate));
      border: 1px solid var(--hu-glass-border-color);
      border-radius: var(--hu-radius-xl);
      box-shadow: var(--hu-shadow-lg);
      animation: hu-modal-enter var(--hu-duration-normal)
        var(--hu-spring-out, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    .panel.closing {
      animation: hu-scale-out var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--hu-space-md) var(--hu-space-lg);
      border-bottom: 1px solid var(--hu-border);
    }

    .heading {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin: 0;
    }

    .close-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-xl);
      height: var(--hu-icon-xl);
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .close-btn svg {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
    }
    .close-btn:hover {
      color: var(--hu-text);
    }

    .close-btn:focus-visible {
      outline: var(--hu-focus-ring-width, 2px) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset, 2px);
      box-shadow: var(--hu-focus-glow-shadow);
      border-radius: var(--hu-radius-sm);
    }

    .body {
      padding: var(--hu-space-lg);
    }
    @media (prefers-reduced-motion: reduce) {
      .backdrop,
      .backdrop.closing,
      .panel,
      .panel.closing {
        animation: none !important;
      }
    }

    @media (prefers-reduced-transparency: reduce) {
      .panel {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-surface-container-highest);
      }
    }
  `;

  @property({ type: Boolean }) open = false;
  @property({ type: String }) heading = "";
  @state() private _closing = false;
  private _closeTimeout: ReturnType<typeof setTimeout> | null = null;
  private _previouslyFocused: HTMLElement | null = null;
  private _scrollY = 0;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this._closing = false;
        if (this._closeTimeout) {
          clearTimeout(this._closeTimeout);
          this._closeTimeout = null;
        }
        this._previouslyFocused = document.activeElement as HTMLElement | null;
        this._lockScroll();
        requestAnimationFrame(() => this._focusFirst());
      } else if (changedProperties.get("open") === true) {
        this._closing = true;
        this._unlockScroll();
        this._closeTimeout = setTimeout(() => {
          this._closing = false;
          this._closeTimeout = null;
        }, 100);
        this._previouslyFocused?.focus();
        this._previouslyFocused = null;
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this._closeTimeout) clearTimeout(this._closeTimeout);
    this._unlockScroll();
  }

  private _lockScroll(): void {
    this._scrollY = window.scrollY;
    document.body.style.overflow = "hidden";
    document.body.style.position = "fixed";
    document.body.style.top = `-${this._scrollY}px`;
    document.body.style.left = "0";
    document.body.style.right = "0";
  }

  private _unlockScroll(): void {
    if (document.body.style.position !== "fixed") return;
    document.body.style.overflow = "";
    document.body.style.position = "";
    document.body.style.top = "";
    document.body.style.left = "";
    document.body.style.right = "";
    window.scrollTo(0, this._scrollY);
  }

  /** Includes `position: fixed` targets; avoids dropping focusables when `offsetParent` is null. */
  private _isFocusableCandidate(el: HTMLElement): boolean {
    if (el.hasAttribute("disabled")) return false;
    if (el.getAttribute("aria-hidden") === "true") return false;
    try {
      const style = window.getComputedStyle(el);
      if (style.display === "none" || style.visibility === "hidden") return false;
      const rect = el.getBoundingClientRect();
      return rect.width > 0 && rect.height > 0;
    } catch {
      return true;
    }
  }

  private _deepestActiveElement(): Element | null {
    let a: Element | null = document.activeElement;
    for (let i = 0; i < 8 && a && "shadowRoot" in a && a.shadowRoot?.activeElement; i++) {
      a = a.shadowRoot.activeElement;
    }
    return a;
  }

  private _getFocusable(): HTMLElement[] {
    const panel = this.renderRoot.querySelector<HTMLElement>(".panel");
    if (!panel) return [];
    const slottedContent = this.renderRoot.querySelector("slot:not([name])")
      ? Array.from(
          (this.renderRoot.querySelector("slot:not([name])") as HTMLSlotElement)?.assignedElements({
            flatten: true,
          }) ?? [],
        )
      : [];
    const selectors = 'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
    const shadowFocusable = Array.from(panel.querySelectorAll<HTMLElement>(selectors));
    const slotFocusable = slottedContent.flatMap((el) => [
      ...(el.matches(selectors) ? [el as HTMLElement] : []),
      ...Array.from(el.querySelectorAll<HTMLElement>(selectors)),
    ]);
    return [...shadowFocusable, ...slotFocusable].filter((el) => this._isFocusableCandidate(el));
  }

  private _focusFirst(): void {
    const focusable = this._getFocusable();
    if (focusable.length > 0) focusable[0].focus();
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      this._close();
      return;
    }
    if (e.key === "Tab") {
      const focusable = this._getFocusable();
      if (focusable.length === 0) {
        e.preventDefault();
        return;
      }
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      const active = this._deepestActiveElement();
      if (e.shiftKey) {
        if (active === first) {
          e.preventDefault();
          last.focus();
        }
      } else if (active === last) {
        e.preventDefault();
        first.focus();
      }
    }
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open && !this._closing) return nothing;

    return html`
      <div
        class="backdrop ${this._closing ? "closing" : ""}"
        role="dialog"
        aria-modal="true"
        aria-labelledby=${this.heading ? "modal-heading" : undefined}
        @click=${this._onBackdropClick}
        @keydown=${this._onKeyDown}
      >
        <div
          class="panel hu-entry-scale ${this._closing ? "closing" : ""}"
          @click=${(e: MouseEvent) => e.stopPropagation()}
        >
          <header class="header">
            ${
              this.heading
                ? html`<h2 id="modal-heading" class="heading">${this.heading}</h2>`
                : nothing
            }
            <button class="close-btn" aria-label="Close" @click=${this._close}>${icons.x}</button>
          </header>
          <div class="body">
            <slot></slot>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-modal": ScModal;
  }
}
