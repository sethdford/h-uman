import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export type CardSurface = "default" | "high" | "highest";

const HU_TILT_MAX_DEG = 8;

@customElement("hu-card")
export class ScCard extends LitElement {
  @property({ type: Boolean, reflect: true }) hoverable = false;
  @property({ type: Boolean, reflect: true }) clickable = false;
  @property({ type: Boolean }) accent = false;
  @property({ type: Boolean }) elevated = false;
  @property({ type: Boolean }) glass = false;
  @property({ type: String, attribute: "aria-label" }) ariaLabelAttr = "";
  /** Opt-in 3D perspective tilt following pointer (overview-style cards). */
  @property({ type: Boolean, reflect: true }) tilt = false;
  /** Enable mesh gradient background overlay. */
  @property({ type: Boolean }) mesh = false;
  /** Enable chromatic prismatic border. */
  @property({ type: Boolean }) chromatic = false;
  /** Animate card into view when it intersects the viewport (IntersectionObserver). */
  @property({ type: Boolean, reflect: true }) entrance = false;
  /** Tonal surface: default (container), high (interactive/elevated), highest (emphasis). */
  @property({ type: String }) surface: CardSurface = "default";

  static override styles = css`
    :host {
      display: block;
    }

    .card {
      position: relative;
      background: var(--hu-card-surface, var(--hu-surface-container));
      background-image: var(--hu-surface-gradient);
      border: 1px solid var(--hu-card-border-color, var(--hu-border-subtle));
      border-radius: var(--hu-radius-xl);
      padding: var(--hu-space-lg);
      box-shadow:
        var(--hu-shadow-card),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 90%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 4%, transparent);
      overflow: hidden;
      container-type: inline-size;
      contain: layout style paint;
    }

    /* Subtle inset highlight — top edge only, Apple-like depth cue */
    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      background: linear-gradient(
        180deg,
        color-mix(in srgb, var(--hu-color-white) 40%, transparent),
        transparent 20%
      );
      mask:
        linear-gradient(var(--hu-color-white) 0 0) content-box,
        linear-gradient(var(--hu-color-white) 0 0);
      mask-composite: exclude;
      -webkit-mask:
        linear-gradient(var(--hu-color-white) 0 0) content-box,
        linear-gradient(var(--hu-color-white) 0 0);
      -webkit-mask-composite: xor;
      padding: 1px;
      pointer-events: none;
      z-index: 1;
    }

    .card > ::slotted(*),
    .card > * {
      position: relative;
      z-index: 1;
    }

    .card.elevated {
      box-shadow:
        var(--hu-shadow-md),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 90%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 4%, transparent);
    }

    /* Accent top-band — gradient bar along top edge */
    .card.accent {
      border-top: none;
      padding-top: calc(var(--hu-space-xl) + var(--hu-space-xs));
    }
    .card.accent::after {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: var(--hu-space-xs);
      background: linear-gradient(
        90deg,
        var(--hu-accent),
        color-mix(in srgb, var(--hu-accent) 40%, transparent)
      );
      border-radius: var(--hu-radius-xl) var(--hu-radius-xl) 0 0;
      pointer-events: none;
      z-index: 1;
    }
    .card.accent::before {
      background: linear-gradient(
        180deg,
        color-mix(in srgb, var(--hu-accent) 6%, transparent),
        transparent 40%
      );
      mask: none;
      -webkit-mask: none;
      padding: 0;
    }

    /* Glass variant — Apple Liquid Glass with specular highlight */
    .card.glass {
      background: color-mix(
        in srgb,
        var(--hu-card-surface, var(--hu-surface-container)) var(--hu-glass-card-bg-mix, 65%),
        transparent
      );
      backdrop-filter: blur(var(--hu-blur-lg)) saturate(var(--hu-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--hu-blur-lg)) saturate(var(--hu-glass-standard-saturate));
      border: 1px solid
        color-mix(in srgb, var(--hu-card-border-color, var(--hu-border-subtle)) 40%, transparent);
      box-shadow:
        var(--hu-shadow-card),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 60%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 3%, transparent);
    }
    .card.glass::before {
      background: radial-gradient(
        ellipse 60% 40% at 25% 0%,
        color-mix(in srgb, var(--hu-color-white) 15%, transparent),
        transparent 70%
      );
      mask: none;
      -webkit-mask: none;
      padding: 0;
    }

    .card.hoverable,
    .card.clickable {
      cursor: pointer;
      will-change: transform, box-shadow;
    }
    :host([hoverable]),
    :host([clickable]) {
      transition:
        transform var(--hu-duration-normal) var(--hu-ease-spring),
        box-shadow var(--hu-duration-normal) var(--hu-ease-out);
    }
    :host([hoverable]:hover),
    :host([clickable]:hover) {
      transform: translateY(var(--hu-physics-card-hover-translateY, -2px));
      box-shadow: var(--hu-shadow-lg);
    }
    :host([hoverable]:active),
    :host([clickable]:active) {
      transform: translateY(0) scale(0.99);
    }
    .card.clickable:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    /* 3D tilt: perspective on host, rotation on inner shell */
    :host([tilt]) {
      perspective: 800px;
    }

    .tilt-inner {
      position: relative;
      z-index: 2;
      transform-style: preserve-3d;
      border-radius: inherit;
      min-height: 100%;
    }

    .tilt-inner > ::slotted(*) {
      position: relative;
      z-index: 2;
    }

    .tilt-light {
      position: absolute;
      inset: 0;
      border-radius: inherit;
      pointer-events: none;
      z-index: 1;
      background: radial-gradient(
        circle farthest-side at var(--hu-tilt-light-x, 50%) var(--hu-tilt-light-y, 50%),
        color-mix(in srgb, var(--hu-color-white) 5%, transparent),
        transparent 62%
      );
    }

    /* Mesh gradient background */
    .card.mesh {
      background-image:
        radial-gradient(
          ellipse at 20% 50%,
          color-mix(in srgb, var(--hu-accent) 6%, transparent) 0%,
          transparent 50%
        ),
        radial-gradient(
          ellipse at 80% 20%,
          color-mix(in srgb, var(--hu-accent-tertiary) 4%, transparent) 0%,
          transparent 50%
        ),
        radial-gradient(
          ellipse at 50% 80%,
          color-mix(in srgb, var(--hu-accent-secondary) 3%, transparent) 0%,
          transparent 50%
        ),
        var(--hu-surface-gradient);
    }

    /* Chromatic prismatic border */
    .card.chromatic::before {
      background: linear-gradient(
        135deg,
        color-mix(in srgb, var(--hu-accent) 15%, transparent),
        color-mix(in srgb, var(--hu-accent-tertiary) 12%, transparent),
        color-mix(in srgb, var(--hu-accent-secondary) 10%, transparent),
        color-mix(in srgb, var(--hu-accent) 15%, transparent)
      );
      mask:
        linear-gradient(var(--hu-color-white) 0 0) content-box,
        linear-gradient(var(--hu-color-white) 0 0);
      mask-composite: exclude;
      -webkit-mask:
        linear-gradient(var(--hu-color-white) 0 0) content-box,
        linear-gradient(var(--hu-color-white) 0 0);
      -webkit-mask-composite: xor;
      padding: 1px;
    }

    :host([entrance]) .card {
      opacity: 0;
      transform: translateY(var(--hu-space-sm, 8px));
      transition:
        opacity var(--hu-duration-normal, 250ms) var(--hu-ease-out),
        transform var(--hu-duration-normal, 250ms) var(--hu-ease-out);
      transition-delay: var(--hu-stagger-delay, 0ms);
    }

    :host([entrance].entered) .card {
      opacity: 1;
      transform: translateY(0);
    }

    @media (prefers-reduced-motion: reduce) {
      :host([entrance]) .card {
        opacity: 1;
        transform: none;
        transition: none;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      :host([hoverable]),
      :host([clickable]),
      :host([tilt]) .tilt-inner {
        transition: none !important;
        animation: none !important;
        transform: none !important;
      }
    }

    @container (max-width: 37.5rem) /* cq-compact */ {
      .card {
        padding: var(--hu-space-adaptive-card-padding, var(--hu-space-md));
      }
    }

    @container (max-width: 17.5rem) {
      .card {
        padding: var(--hu-space-sm);
        border-radius: var(--hu-radius-md);
      }
    }

    @media (prefers-contrast: more) {
      .card {
        border-width: 2px;
        border-color: var(--hu-border);
        box-shadow: none;
      }
    }
  `;

  private _entranceObserver: IntersectionObserver | null = null;
  private _tiltVisibilityObserver: IntersectionObserver | null = null;
  private _tiltIntersecting = false;
  private _tiltTracking = false;
  private _tiltRaf: number | null = null;
  private _tiltPointer: { x: number; y: number } | null = null;

  private _tiltSupported(): boolean {
    if (typeof window === "undefined") return false;
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return false;
    if (window.matchMedia("(hover: none)").matches) return false;
    return true;
  }

  private _syncTiltIntersecting(): void {
    const r = this.getBoundingClientRect();
    const vh = window.innerHeight;
    const vw = window.innerWidth;
    this._tiltIntersecting =
      r.width > 0 && r.height > 0 && r.bottom > 0 && r.right > 0 && r.top < vh && r.left < vw;
  }

  private _teardownTiltVisibilityObserver(): void {
    if (this._tiltVisibilityObserver) {
      this._tiltVisibilityObserver.disconnect();
      this._tiltVisibilityObserver = null;
    }
    this._tiltIntersecting = false;
  }

  private _setupTiltVisibilityObserver(): void {
    this._teardownTiltVisibilityObserver();
    if (!this.tilt || !this._tiltSupported()) {
      return;
    }
    if (typeof IntersectionObserver === "undefined") {
      this._syncTiltIntersecting();
      return;
    }
    this._tiltVisibilityObserver = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.target === this) {
            this._tiltIntersecting = entry.isIntersecting;
            if (!entry.isIntersecting) {
              this._resetTiltImmediate();
            }
          }
        }
      },
      { threshold: 0 },
    );
    this._tiltVisibilityObserver.observe(this);
    this._syncTiltIntersecting();
  }

  private _getTiltInner(): HTMLElement | null {
    return this.shadowRoot?.querySelector(".tilt-inner") ?? null;
  }

  private _cancelTiltRaf(): void {
    if (this._tiltRaf !== null) {
      cancelAnimationFrame(this._tiltRaf);
      this._tiltRaf = null;
    }
    this._tiltPointer = null;
  }

  private _resetTiltImmediate(): void {
    this._tiltTracking = false;
    this._cancelTiltRaf();
    const inner = this._getTiltInner();
    if (!inner) return;
    inner.style.willChange = "";
    inner.style.transition = "";
    inner.style.transform = "";
    inner.style.removeProperty("--hu-tilt-light-x");
    inner.style.removeProperty("--hu-tilt-light-y");
  }

  private _onTiltPointerEnter = (e: PointerEvent): void => {
    if (!this.tilt || !this._tiltSupported() || !this._tiltIntersecting) return;
    const inner = this._getTiltInner();
    if (!inner) return;
    this._tiltTracking = true;
    inner.style.transition = "none";
    inner.style.willChange = "transform";
    this._tiltPointer = { x: e.clientX, y: e.clientY };
    if (this._tiltRaf === null) {
      this._tiltRaf = requestAnimationFrame(() => this._applyTiltFrame());
    }
  };

  private _onTiltPointerMove = (e: PointerEvent): void => {
    if (!this.tilt || !this._tiltSupported() || !this._tiltIntersecting || !this._tiltTracking) {
      return;
    }
    this._tiltPointer = { x: e.clientX, y: e.clientY };
    if (this._tiltRaf === null) {
      this._tiltRaf = requestAnimationFrame(() => this._applyTiltFrame());
    }
  };

  private _onTiltPointerLeave = (): void => {
    if (!this.tilt) return;
    this._tiltTracking = false;
    this._cancelTiltRaf();
    const inner = this._getTiltInner();
    if (!inner || !this._tiltSupported()) return;
    inner.style.willChange = "";
    inner.style.transition = `transform var(--hu-duration-normal) var(--hu-ease-spring)`;
    inner.style.transform = "rotateX(0deg) rotateY(0deg)";
    inner.style.setProperty("--hu-tilt-light-x", "50%");
    inner.style.setProperty("--hu-tilt-light-y", "50%");
  };

  private _applyTiltFrame(): void {
    this._tiltRaf = null;
    if (
      !this.tilt ||
      !this._tiltSupported() ||
      !this._tiltIntersecting ||
      !this._tiltTracking ||
      !this._tiltPointer
    ) {
      return;
    }
    const card = this.shadowRoot?.querySelector(".card") as HTMLElement | null;
    const inner = this._getTiltInner();
    if (!card || !inner) return;

    const rect = card.getBoundingClientRect();
    const halfW = Math.max(rect.width / 2, 1);
    const halfH = Math.max(rect.height / 2, 1);
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;

    const rotateY = ((this._tiltPointer.x - cx) / halfW) * HU_TILT_MAX_DEG;
    const rotateX = (-(this._tiltPointer.y - cy) / halfH) * HU_TILT_MAX_DEG;

    inner.style.transform = `rotateX(${rotateX}deg) rotateY(${rotateY}deg)`;

    const lx = ((this._tiltPointer.x - rect.left) / Math.max(rect.width, 1)) * 100;
    const ly = ((this._tiltPointer.y - rect.top) / Math.max(rect.height, 1)) * 100;
    inner.style.setProperty("--hu-tilt-light-x", `${lx}%`);
    inner.style.setProperty("--hu-tilt-light-y", `${ly}%`);
  }

  private _teardownEntranceObserver(): void {
    if (this._entranceObserver) {
      this._entranceObserver.disconnect();
      this._entranceObserver = null;
    }
  }

  private _setupEntranceObserver(): void {
    this._teardownEntranceObserver();
    if (!this.entrance) {
      return;
    }
    if (typeof IntersectionObserver === "undefined") {
      this.classList.add("entered");
      return;
    }
    this._entranceObserver = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            this.classList.add("entered");
            this._teardownEntranceObserver();
            break;
          }
        }
      },
      { threshold: 0.1 },
    );
    this._entranceObserver.observe(this);
  }

  override connectedCallback(): void {
    super.connectedCallback();
    if (this.tilt) {
      this._setupTiltVisibilityObserver();
    }
    if (this.entrance) {
      this._setupEntranceObserver();
    }
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this._resetTiltImmediate();
    this._teardownTiltVisibilityObserver();
    this._teardownEntranceObserver();
  }

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("tilt")) {
      this._resetTiltImmediate();
      if (this.tilt) {
        this._setupTiltVisibilityObserver();
      } else {
        this._teardownTiltVisibilityObserver();
      }
    }
    if (changed.has("entrance")) {
      if (this.entrance) {
        this._setupEntranceObserver();
      } else {
        this._teardownEntranceObserver();
        this.classList.remove("entered");
      }
    }
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.clickable || (e.key !== "Enter" && e.key !== " ")) return;
    e.preventDefault();
    this.dispatchEvent(new MouseEvent("click", { bubbles: true, composed: true }));
  }

  render() {
    const surfaceToken =
      this.surface === "high"
        ? "var(--hu-surface-container-high)"
        : this.surface === "highest"
          ? "var(--hu-surface-container-highest)"
          : "var(--hu-surface-container)";

    const classes = [
      "card",
      this.hoverable ? "hoverable" : "",
      this.clickable ? "clickable" : "",
      this.accent ? "accent" : "",
      this.elevated ? "elevated" : "",
      this.glass ? "glass" : "",
      this.mesh ? "mesh" : "",
      this.chromatic ? "chromatic" : "",
    ]
      .filter(Boolean)
      .join(" ");

    const onTiltEnter = this.tilt ? this._onTiltPointerEnter : undefined;
    const onTiltMove = this.tilt ? this._onTiltPointerMove : undefined;
    const onTiltLeave = this.tilt ? this._onTiltPointerLeave : undefined;

    return html`
      <div
        class=${classes}
        style="--hu-card-surface: ${surfaceToken}"
        role=${this.clickable ? "button" : undefined}
        tabindex=${this.clickable ? 0 : undefined}
        aria-label=${this.clickable && this.ariaLabelAttr ? this.ariaLabelAttr : undefined}
        @keydown=${this._onKeyDown}
        @pointerenter=${onTiltEnter}
        @pointermove=${onTiltMove}
        @pointerleave=${onTiltLeave}
      >
        ${
          this.tilt
            ? html`
                <div class="tilt-inner">
                  <div class="tilt-light" aria-hidden="true"></div>
                  <slot></slot>
                </div>
              `
            : html`<slot></slot>`
        }
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-card": ScCard;
  }
}
