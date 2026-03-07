import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type SheetSize = "sm" | "md" | "lg" | "full";
type SnapPoint = "collapsed" | "half" | "full";

const SIZE_MAP: Record<SheetSize, string> = {
  sm: "30vh",
  md: "50vh",
  lg: "75vh",
  full: "95vh",
};

@customElement("sc-sheet")
export class ScSheet extends LitElement {
  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 9998;
      background: color-mix(in srgb, var(--sc-bg) 50%, transparent);
      backdrop-filter: blur(var(--sc-blur-sm, 4px));
      -webkit-backdrop-filter: blur(var(--sc-blur-sm, 4px));
      opacity: 0;
      visibility: hidden;
      transition:
        opacity var(--sc-duration-normal) var(--sc-ease-out),
        visibility var(--sc-duration-normal);
    }

    .backdrop.open {
      opacity: 1;
      visibility: visible;
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .panel {
      position: fixed;
      bottom: 0;
      left: 0;
      right: 0;
      max-height: 95vh;
      background: var(--sc-bg-overlay);
      border-radius: var(--sc-radius-xl) var(--sc-radius-xl) 0 0;
      box-shadow: var(--sc-shadow-lg);
      display: flex;
      flex-direction: column;
      transform: translateY(100%);
      z-index: 9999;
    }

    .panel.open {
      animation: sc-sheet-enter var(--sc-duration-slow)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
      transform: translateY(0);
    }

    @media (prefers-reduced-motion: reduce) {
      .backdrop,
      .panel {
        transition: none !important;
        animation: none !important;
      }
    }

    .handle-wrapper {
      display: flex;
      justify-content: center;
      padding: var(--sc-space-sm) 0;
      cursor: grab;
      touch-action: none;
    }

    .handle-wrapper:active {
      cursor: grabbing;
    }

    .handle {
      width: 32px;
      height: 4px;
      background: var(--sc-border);
      border-radius: var(--sc-radius-full);
    }

    .content {
      flex: 1;
      overflow: auto;
      padding: 0 var(--sc-space-lg) var(--sc-space-lg);
    }
  `;

  @property({ type: Boolean, reflect: true }) open = false;
  @property({ type: String }) size: SheetSize = "md";

  @state() private _snapPoint: SnapPoint = "half";
  @state() private _dragStartY = 0;
  @state() private _dragStartHeight = 0;
  private _focusableSelector =
    'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
  private _previousActiveElement: HTMLElement | null = null;
  private _keyHandler = this._onKeyDown.bind(this);

  private get _height(): string {
    switch (this._snapPoint) {
      case "collapsed":
        return "min(15vh, 120px)";
      case "half":
        return SIZE_MAP[this.size];
      case "full":
        return "95vh";
      default:
        return SIZE_MAP[this.size];
    }
  }

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open")) {
      if (this.open) {
        this._previousActiveElement = document.activeElement as HTMLElement | null;
        document.addEventListener("keydown", this._keyHandler);
        this._snapPoint = "half";
        requestAnimationFrame(() => this._trapFocus());
      } else {
        document.removeEventListener("keydown", this._keyHandler);
        this._restoreFocus();
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    document.removeEventListener("keydown", this._keyHandler);
  }

  private _getFocusableElements(): HTMLElement[] {
    const root = this.renderRoot;
    return Array.from(root.querySelectorAll<HTMLElement>(this._focusableSelector));
  }

  private _trapFocus(): void {
    const focusable = this._getFocusableElements();
    if (focusable.length > 0) {
      focusable[0].focus();
    }
  }

  private _restoreFocus(): void {
    if (this._previousActiveElement?.focus) {
      this._previousActiveElement.focus();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.open) return;
    if (e.key === "Escape") {
      e.preventDefault();
      this._close();
      return;
    }
    if (e.key !== "Tab") return;
    const focusable = this._getFocusableElements();
    if (focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (e.shiftKey) {
      if (document.activeElement === first) {
        e.preventDefault();
        last.focus();
      }
    } else if (document.activeElement === last) {
      e.preventDefault();
      first.focus();
    }
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  private _onHandlePointerDown(e: PointerEvent): void {
    const target = e.currentTarget as HTMLElement;
    e.preventDefault();
    this._dragStartY = e.clientY;
    const panel = this.renderRoot.querySelector<HTMLElement>(".panel");
    this._dragStartHeight = panel ? panel.getBoundingClientRect().height : 0;
    target.setPointerCapture(e.pointerId);
    target.addEventListener("pointermove", this._onHandlePointerMove);
    const onUp = (): void => {
      target.removeEventListener("pointermove", this._onHandlePointerMove);
      target.releasePointerCapture(e.pointerId);
      target.removeEventListener("pointerup", onUp);
    };
    target.addEventListener("pointerup", onUp);
  }

  private _onHandlePointerMove = (e: PointerEvent): void => {
    const dy = this._dragStartY - e.clientY;
    const viewHeight = window.innerHeight;
    const newHeight = this._dragStartHeight + dy;
    const ratio = newHeight / viewHeight;
    if (ratio < 0.25) {
      this._snapPoint = "collapsed";
    } else if (ratio < 0.6) {
      this._snapPoint = "half";
    } else {
      this._snapPoint = "full";
    }
  };

  override render() {
    if (!this.open) return nothing;

    return html`
      <div
        class="backdrop open"
        role="dialog"
        aria-modal="true"
        aria-label="Sheet"
        @click=${this._onBackdropClick}
      >
        <div
          class="panel open"
          role="document"
          style="height: ${this._height}; max-height: ${this._height};"
          @click=${(e: MouseEvent) => e.stopPropagation()}
        >
          <div
            class="handle-wrapper"
            role="button"
            tabindex="0"
            aria-label="Drag to resize or dismiss"
            @pointerdown=${this._onHandlePointerDown}
            @keydown=${(e: KeyboardEvent) => {
              if (e.key === "Enter" || e.key === " ") {
                e.preventDefault();
                this._close();
              }
            }}
          >
            <div class="handle" aria-hidden="true"></div>
          </div>
          <div class="content">
            <slot></slot>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-sheet": ScSheet;
  }
}
