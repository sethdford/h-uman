/**
 * Gesture-driven animation utilities for Lit components.
 * Uses PointerEvent for cross-device support (mouse + touch).
 * Respects prefers-reduced-motion.
 */

import type { ReactiveController, ReactiveControllerHost } from "lit";

/** Returns true if user prefers reduced motion. */
export function prefersReducedMotion(): boolean {
  if (typeof window === "undefined" || !window.matchMedia) return false;
  return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

// ─── rippleEffect ─────────────────────────────────────────────────────────

/**
 * Creates an expanding circle ripple animation from the pointer position.
 * Uses hu-ripple keyframe (defined in theme.css). Self-cleaning: removes the ripple element after animation.
 */
export function rippleEffect(element: HTMLElement, event: PointerEvent): void {
  if (prefersReducedMotion()) return;

  const rect = element.getBoundingClientRect();
  const x = event.clientX - rect.left;
  const y = event.clientY - rect.top;

  const ripple = document.createElement("span");
  ripple.setAttribute("aria-hidden", "true");
  ripple.style.cssText = `
    position: absolute;
    left: ${x}px;
    top: ${y}px;
    width: 1.25rem;
    height: 1.25rem;
    margin-left: calc(-1 * var(--hu-space-md));
    margin-top: calc(-1 * var(--hu-space-md));
    border-radius: 50%;
    background: currentColor;
    opacity: 0.4;
    pointer-events: none;
    transform: scale(0);
    animation: hu-ripple var(--hu-duration-slow) var(--hu-ease-out, ease-out) forwards;
  `;

  const pos = element.style.position;
  if (pos === "static" || !pos) element.style.position = "relative";
  element.style.overflow = "hidden";
  element.appendChild(ripple);

  const duration = 350; // fallback if --hu-duration-slow unavailable
  setTimeout(() => {
    ripple.remove();
  }, duration);
}

// ─── DragDismissController ─────────────────────────────────────────────────

const DRAG_DISMISS_THRESHOLD = 100;
const RUBBER_BAND_FACTOR = 0.3;

/**
 * ReactiveController that enables vertical drag-to-dismiss on the host element.
 * When dragged past threshold (100px), fires 'dismiss' event.
 * Uses rubber-band physics when pulled beyond boundary.
 * Respects prefers-reduced-motion.
 */
export class DragDismissController implements ReactiveController {
  private host: ReactiveControllerHost & HTMLElement;
  private rafId: number | null = null;
  private pointerId: number | null = null;
  private startY = 0;
  private currentY = 0;
  private boundHandlers: {
    down: (e: PointerEvent) => void;
    move: (e: PointerEvent) => void;
    up: (e: PointerEvent) => void;
    cancel: (e: PointerEvent) => void;
  };

  constructor(host: ReactiveControllerHost & HTMLElement) {
    this.host = host;
    this.boundHandlers = {
      down: this.onPointerDown.bind(this),
      move: this.onPointerMove.bind(this),
      up: this.onPointerUp.bind(this),
      cancel: this.onPointerCancel.bind(this),
    };
    host.addController(this);
  }

  hostConnected(): void {
    const el = this.host;
    el.addEventListener("pointerdown", this.boundHandlers.down, { passive: true });
    el.addEventListener("pointermove", this.boundHandlers.move, { passive: false });
    el.addEventListener("pointerup", this.boundHandlers.up, { passive: true });
    el.addEventListener("pointercancel", this.boundHandlers.cancel, { passive: true });
  }

  hostDisconnected(): void {
    const el = this.host;
    el.removeEventListener("pointerdown", this.boundHandlers.down);
    el.removeEventListener("pointermove", this.boundHandlers.move);
    el.removeEventListener("pointerup", this.boundHandlers.up);
    el.removeEventListener("pointercancel", this.boundHandlers.cancel);
    if (this.rafId !== null) cancelAnimationFrame(this.rafId);
  }

  private onPointerDown(e: PointerEvent): void {
    if (this.pointerId !== null || prefersReducedMotion()) return;
    this.pointerId = e.pointerId;
    this.startY = e.clientY;
    this.currentY = e.clientY;
    (this.host as HTMLElement).setPointerCapture(e.pointerId);
  }

  private onPointerMove(e: PointerEvent): void {
    if (this.pointerId !== e.pointerId) return;
    e.preventDefault();
    const dy = e.clientY - this.startY;
    if (dy <= 0) return; // Only track downward drag for dismiss
    this.currentY = e.clientY;
    this.applyTransform(dy);
  }

  private applyTransform(dy: number): void {
    if (prefersReducedMotion()) return;
    let ty: number;
    if (dy <= DRAG_DISMISS_THRESHOLD) {
      ty = dy;
    } else {
      const overflow = dy - DRAG_DISMISS_THRESHOLD;
      ty = DRAG_DISMISS_THRESHOLD + overflow * RUBBER_BAND_FACTOR;
    }
    if (this.rafId !== null) cancelAnimationFrame(this.rafId);
    this.rafId = requestAnimationFrame(() => {
      this.host.style.transform = `translateY(${ty}px)`;
      this.host.style.transition = "none";
      this.rafId = null;
    });
  }

  private onPointerUp(e: PointerEvent): void {
    if (this.pointerId !== e.pointerId) return;
    (this.host as HTMLElement).releasePointerCapture(e.pointerId);
    const dy = this.currentY - this.startY;
    if (dy >= DRAG_DISMISS_THRESHOLD) {
      this.host.dispatchEvent(new CustomEvent("dismiss", { bubbles: true, composed: true }));
    }
    this.resetTransform();
    this.pointerId = null;
  }

  private onPointerCancel(): void {
    this.pointerId = null;
    this.resetTransform();
  }

  private resetTransform(): void {
    if (prefersReducedMotion()) {
      this.host.style.transform = "";
      this.host.style.transition = "";
      return;
    }
    this.host.style.transition = `transform var(--hu-duration-normal, 200ms) var(--hu-ease-out, ease-out)`;
    this.host.style.transform = "";
  }
}

// ─── SwipeController ─────────────────────────────────────────────────────

const SWIPE_MIN_DISTANCE = 50;
const SWIPE_VELOCITY_THRESHOLD = 0.3;

export type SwipeDirection = "swipe-left" | "swipe-right" | "swipe-up" | "swipe-down";

export interface SwipeControllerOptions {
  minDistance?: number;
  velocityThreshold?: number;
}

/**
 * Tracks horizontal/vertical swipe gestures on the host element.
 * Fires 'swipe-left', 'swipe-right', 'swipe-up', 'swipe-down' custom events.
 * Minimum distance: 50px. Velocity threshold: 0.3px/ms.
 */
export class SwipeController implements ReactiveController {
  private host: ReactiveControllerHost & HTMLElement;
  private options: Required<SwipeControllerOptions>;
  private startX = 0;
  private startY = 0;
  private startTime = 0;
  private pointerId: number | null = null;
  private boundHandlers: {
    down: (e: PointerEvent) => void;
    up: (e: PointerEvent) => void;
  };

  constructor(host: ReactiveControllerHost & HTMLElement, options: SwipeControllerOptions = {}) {
    this.host = host;
    this.options = {
      minDistance: options.minDistance ?? SWIPE_MIN_DISTANCE,
      velocityThreshold: options.velocityThreshold ?? SWIPE_VELOCITY_THRESHOLD,
    };
    this.boundHandlers = {
      down: this.onPointerDown.bind(this),
      up: this.onPointerUp.bind(this),
    };
    host.addController(this);
  }

  hostConnected(): void {
    const el = this.host;
    el.addEventListener("pointerdown", this.boundHandlers.down, { passive: true });
    el.addEventListener("pointerup", this.boundHandlers.up, { passive: true });
  }

  hostDisconnected(): void {
    const el = this.host;
    el.removeEventListener("pointerdown", this.boundHandlers.down);
    el.removeEventListener("pointerup", this.boundHandlers.up);
  }

  private onPointerDown(e: PointerEvent): void {
    if (this.pointerId !== null) return;
    this.pointerId = e.pointerId;
    this.startX = e.clientX;
    this.startY = e.clientY;
    this.startTime = performance.now();
  }

  private onPointerUp(e: PointerEvent): void {
    if (this.pointerId !== e.pointerId) return;
    const dx = e.clientX - this.startX;
    const dy = e.clientY - this.startY;
    const dt = performance.now() - this.startTime;
    this.pointerId = null;

    if (dt <= 0) return;
    const vx = Math.abs(dx) / dt;
    const vy = Math.abs(dy) / dt;

    const minDist = this.options.minDistance;
    const velThresh = this.options.velocityThreshold;

    if (Math.abs(dx) > Math.abs(dy)) {
      if (Math.abs(dx) >= minDist && vx >= velThresh) {
        const dir: SwipeDirection = dx > 0 ? "swipe-right" : "swipe-left";
        this.host.dispatchEvent(
          new CustomEvent(dir, { bubbles: true, composed: true, detail: { dx, dy, velocity: vx } }),
        );
      }
    } else {
      if (Math.abs(dy) >= minDist && vy >= velThresh) {
        const dir: SwipeDirection = dy > 0 ? "swipe-down" : "swipe-up";
        this.host.dispatchEvent(
          new CustomEvent(dir, { bubbles: true, composed: true, detail: { dx, dy, velocity: vy } }),
        );
      }
    }
  }
}

// ─── PullRefreshController ───────────────────────────────────────────────

const PULL_REFRESH_THRESHOLD = 80;
const PULL_RUBBER_FACTOR = 0.4;

/**
 * Attaches to a scrollable container for pull-to-refresh.
 * When scrolled to top and pulled down, shows loading indicator.
 * Uses rubber-band overshoot physics.
 * Fires 'refresh' custom event when released past threshold.
 */
export class PullRefreshController implements ReactiveController {
  private host: ReactiveControllerHost & HTMLElement;
  private scrollTarget: HTMLElement;
  private indicatorElement: HTMLElement | null = null;
  private startY = 0;
  private currentY = 0;
  private pointerId: number | null = null;
  private boundHandlers: {
    down: (e: PointerEvent) => void;
    move: (e: PointerEvent) => void;
    up: (e: PointerEvent) => void;
    cancel: (e: PointerEvent) => void;
  };

  constructor(host: ReactiveControllerHost & HTMLElement, scrollTarget: HTMLElement) {
    this.host = host;
    this.scrollTarget = scrollTarget;
    this.boundHandlers = {
      down: this.onPointerDown.bind(this),
      move: this.onPointerMove.bind(this),
      up: this.onPointerUp.bind(this),
      cancel: this.onPointerCancel.bind(this),
    };
    host.addController(this);
  }

  hostConnected(): void {
    const el = this.scrollTarget;
    el.addEventListener("pointerdown", this.boundHandlers.down, { passive: true });
    el.addEventListener("pointermove", this.boundHandlers.move, { passive: false });
    el.addEventListener("pointerup", this.boundHandlers.up, { passive: true });
    el.addEventListener("pointercancel", this.boundHandlers.cancel, { passive: true });
  }

  hostDisconnected(): void {
    const el = this.scrollTarget;
    el.removeEventListener("pointerdown", this.boundHandlers.down);
    el.removeEventListener("pointermove", this.boundHandlers.move);
    el.removeEventListener("pointerup", this.boundHandlers.up);
    el.removeEventListener("pointercancel", this.boundHandlers.cancel);
    this.indicatorElement?.remove();
  }

  private get isAtTop(): boolean {
    return this.scrollTarget.scrollTop <= 0;
  }

  private onPointerDown(e: PointerEvent): void {
    if (this.pointerId !== null) return;
    if (!this.isAtTop) return;
    this.pointerId = e.pointerId;
    this.startY = e.clientY;
    this.currentY = e.clientY;
    this.scrollTarget.setPointerCapture(e.pointerId);
  }

  private onPointerMove(e: PointerEvent): void {
    if (this.pointerId !== e.pointerId) return;
    if (!this.isAtTop) {
      this.pointerId = null;
      this.scrollTarget.releasePointerCapture(e.pointerId);
      this.reset();
      return;
    }
    e.preventDefault();
    this.currentY = e.clientY;
    const pull = this.currentY - this.startY;
    if (pull <= 0) return;
    this.applyPull(pull);
  }

  private applyPull(pull: number): void {
    const reduced = prefersReducedMotion();
    if (reduced) return;

    let offset: number;
    if (pull <= PULL_REFRESH_THRESHOLD) {
      offset = pull;
    } else {
      const overflow = pull - PULL_REFRESH_THRESHOLD;
      offset = PULL_REFRESH_THRESHOLD + overflow * PULL_RUBBER_FACTOR;
    }

    this.ensureIndicator();
    if (this.indicatorElement) {
      this.indicatorElement.style.opacity = String(Math.min(1, offset / PULL_REFRESH_THRESHOLD));
      this.indicatorElement.style.transform = `translateY(${offset}px)`;
    }
  }

  private ensureIndicator(): void {
    if (this.indicatorElement && this.host.contains(this.indicatorElement)) return;
    const indicator = document.createElement("div");
    indicator.setAttribute("aria-hidden", "true");
    indicator.style.cssText = `
      position: absolute;
      top: 0;
      left: 50%;
      transform: translate(-50%, -100%);
      padding: var(--hu-space-md, 1rem);
      font-size: var(--hu-text-sm, 0.8125rem);
      color: var(--hu-text-muted);
      opacity: 0;
      transition: opacity var(--hu-duration-fast, 100ms) var(--hu-ease-out);
      pointer-events: none;
    `;
    indicator.textContent = "Pull to refresh";
    const wrapper = this.scrollTarget.parentElement ?? this.host;
    const pos = getComputedStyle(wrapper).position;
    if (pos === "static" || !pos) wrapper.style.position = "relative";
    wrapper.insertBefore(indicator, this.scrollTarget);
    this.indicatorElement = indicator;
  }

  private onPointerUp(e: PointerEvent): void {
    if (this.pointerId !== e.pointerId) return;
    this.scrollTarget.releasePointerCapture(e.pointerId);
    const pull = this.currentY - this.startY;
    if (pull >= PULL_REFRESH_THRESHOLD) {
      this.host.dispatchEvent(new CustomEvent("refresh", { bubbles: true, composed: true }));
    }
    this.reset();
    this.pointerId = null;
  }

  private onPointerCancel(): void {
    this.pointerId = null;
    this.reset();
  }

  private reset(): void {
    if (prefersReducedMotion()) return;
    if (this.indicatorElement) {
      this.indicatorElement.style.opacity = "0";
      this.indicatorElement.style.transform = "translate(-50%, -100%)";
      this.indicatorElement.style.transition = `opacity var(--hu-duration-fast, 100ms) var(--hu-ease-out), transform var(--hu-duration-normal, 200ms) var(--hu-ease-out, ease-out)`;
      setTimeout(() => {
        this.indicatorElement?.remove();
        this.indicatorElement = null;
      }, 250);
    }
  }
}
