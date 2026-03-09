import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import { springEntry } from "../lib/spring.js";

@customElement("sc-image-viewer")
export class ScImageViewer extends LitElement {
  @property({ type: String }) src = "";
  @property({ type: Boolean }) open = false;

  @state() private _closing = false;
  @state() private _scale = 1;
  @state() private _translateX = 0;
  @state() private _translateY = 0;
  @state() private _isPanning = false;
  @state() private _panStart = { x: 0, y: 0, tx: 0, ty: 0 };

  private _closeTimeout: ReturnType<typeof setTimeout> | null = null;

  static override styles = css`
    :host {
      display: contents;
    }

    .backdrop {
      position: fixed;
      inset: 0;
      z-index: 9999;
      background: color-mix(in srgb, var(--sc-bg) 30%, transparent);
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--sc-space-xl);
      box-sizing: border-box;
      cursor: zoom-out;
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out);
    }

    .backdrop.closing {
      animation: sc-fade-out var(--sc-duration-fast) var(--sc-ease-in) forwards;
    }

    .close-btn {
      position: absolute;
      top: var(--sc-space-md);
      right: var(--sc-space-md);
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-xl);
      height: var(--sc-icon-xl);
      padding: 0;
      background: color-mix(in srgb, var(--sc-surface-container-highest) 90%, transparent);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      color: var(--sc-text);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out);
      z-index: 1;
    }

    .close-btn:hover {
      background: var(--sc-hover-overlay);
      color: var(--sc-text);
    }

    .close-btn:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .close-btn svg {
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
    }

    .image-wrap {
      max-width: 90vw;
      max-height: 90vh;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: grab;
    }

    .image-wrap:active {
      cursor: grabbing;
    }

    .image-wrap img {
      max-width: 100%;
      max-height: 90vh;
      object-fit: contain;
      border-radius: var(--sc-radius-md);
      box-shadow: var(--sc-shadow-lg);
      user-select: none;
      -webkit-user-drag: none;
    }

    @media (prefers-reduced-motion: reduce) {
      .backdrop,
      .backdrop.closing {
        animation: none;
      }
    }
  `;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("open") || changedProperties.has("src")) {
      if (this.open && this.src) {
        this._closing = false;
        this._scale = 1;
        this._translateX = 0;
        this._translateY = 0;
        if (this._closeTimeout) {
          clearTimeout(this._closeTimeout);
          this._closeTimeout = null;
        }
        requestAnimationFrame(() => {
          this._playEnterAnimation();
          const btn = this.renderRoot.querySelector<HTMLButtonElement>(".close-btn");
          btn?.focus();
        });
      } else if (changedProperties.get("open") === true) {
        this._closing = true;
        this._closeTimeout = setTimeout(() => {
          this._closing = false;
          this._closeTimeout = null;
        }, 150);
      }
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this._closeTimeout) clearTimeout(this._closeTimeout);
  }

  private _playEnterAnimation(): void {
    const wrap = this.renderRoot.querySelector<HTMLElement>(".image-wrap");
    if (!wrap) return;
    springEntry(wrap, 20).promise.catch(() => {});
  }

  private _onBackdropClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
      this._close();
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      this._close();
    }
  }

  private _onWheel(e: WheelEvent): void {
    e.preventDefault();
    const delta = e.deltaY > 0 ? -0.1 : 0.1;
    this._scale = Math.max(0.5, Math.min(4, this._scale + delta));
    this.requestUpdate();
  }

  private _onPointerDown(e: PointerEvent): void {
    if (this._scale <= 1) return;
    this._isPanning = true;
    this._panStart = {
      x: e.clientX,
      y: e.clientY,
      tx: this._translateX,
      ty: this._translateY,
    };
    (e.target as HTMLElement).setPointerCapture(e.pointerId);
  }

  private _onPointerMove(e: PointerEvent): void {
    if (!this._isPanning) return;
    this._translateX = this._panStart.tx + (e.clientX - this._panStart.x);
    this._translateY = this._panStart.ty + (e.clientY - this._panStart.y);
    this.requestUpdate();
  }

  private _onPointerUp(e: PointerEvent): void {
    this._isPanning = false;
    (e.target as HTMLElement).releasePointerCapture(e.pointerId);
  }

  private _close(): void {
    this.dispatchEvent(new CustomEvent("close", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this.open && !this._closing) return nothing;
    if (!this.src) return nothing;

    const transform = `scale(${this._scale}) translate(${this._translateX}px, ${this._translateY}px)`;

    return html`
      <div
        class="backdrop ${this._closing ? "closing" : ""}"
        role="dialog"
        aria-modal="true"
        aria-label="Image viewer"
        @click=${this._onBackdropClick}
        @keydown=${this._onKeyDown}
        @wheel=${this._onWheel}
      >
        <button class="close-btn" aria-label="Close" @click=${this._close}>${icons.x}</button>
        <div
          class="image-wrap"
          style="transform: ${transform};"
          @pointerdown=${this._onPointerDown}
          @pointermove=${this._onPointerMove}
          @pointerup=${this._onPointerUp}
          @pointercancel=${this._onPointerUp}
          @click=${(e: MouseEvent) => e.stopPropagation()}
        >
          <img src=${this.src} alt="" loading="eager" draggable="false" />
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-image-viewer": ScImageViewer;
  }
}
