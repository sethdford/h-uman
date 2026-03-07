/**
 * SeaClaw UI animation engine.
 * Uses Web Animations API; respects prefers-reduced-motion.
 */

import {
  springToLinearEasing,
  SPRING_PRESETS,
  type SpringConfig,
  type SpringPresetName,
} from "./spring.js";

export { SPRING_PRESETS } from "./spring.js";
export type { SpringConfig, SpringPresetName } from "./spring.js";

/** Returns true if user prefers reduced motion. */
export function prefersReducedMotion(): boolean {
  if (typeof window === "undefined" || !window.matchMedia) return false;
  return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

/** Matches --sc-duration-normal (200ms). */
const DURATION_NORMAL = 200;
/** Matches --sc-duration-slow (350ms). */
const DURATION_SLOW = 350;
/** Matches --sc-duration-moderate (300ms). */
const DURATION_MODERATE = 300;
const DEFAULT_DURATION = DURATION_NORMAL;

function withReducedMotion<T>(fn: () => T, fallback: () => T): T {
  return prefersReducedMotion() ? fallback() : fn();
}

/**
 * Applies spring-based transition via Web Animations API.
 * properties: record of CSS property to [from, to] values.
 */
export function springTransition(
  element: Element,
  properties: Record<string, [string, string]>,
  config?: SpringConfig | SpringPresetName,
): Animation {
  const springConfig =
    typeof config === "string" ? SPRING_PRESETS[config] : (config ?? SPRING_PRESETS.standard);
  const easing = springToLinearEasing(springConfig);

  const keyframes: Record<string, (string | number)[]> = {};
  for (const [prop, [from, to]] of Object.entries(properties)) {
    keyframes[prop] = [from, to];
  }

  const kf = keyframes as PropertyIndexedKeyframes;
  return withReducedMotion(
    () =>
      element.animate(kf, {
        duration: DURATION_SLOW,
        easing,
        fill: "forwards",
      }),
    () =>
      element.animate(kf, {
        duration: 0,
        fill: "forwards",
      }),
  );
}

/**
 * FLIP layout animation: record position → callback (layout change) → animate to new position.
 */
export function flipAnimate(
  element: Element,
  callback: () => void,
  config?: SpringConfig | SpringPresetName,
): Promise<void> {
  const springConfig =
    typeof config === "string" ? SPRING_PRESETS[config] : (config ?? SPRING_PRESETS.standard);
  const easing = springToLinearEasing(springConfig);

  return withReducedMotion(
    () => {
      const rect = element.getBoundingClientRect();
      callback();
      return new Promise<void>((resolve) => {
        requestAnimationFrame(() => {
          const newRect = element.getBoundingClientRect();
          const deltaX = rect.left - newRect.left;
          const deltaY = rect.top - newRect.top;

          if (deltaX === 0 && deltaY === 0) {
            resolve();
            return;
          }

          const anim = element.animate(
            [
              { transform: `translate(${deltaX}px, ${deltaY}px)` },
              { transform: "translate(0, 0)" },
            ],
            {
              duration: DURATION_MODERATE,
              easing,
              fill: "forwards",
            },
          );
          anim.finished.then(() => resolve());
        });
      });
    },
    () => {
      callback();
      return Promise.resolve();
    },
  );
}

export interface StaggerConfig {
  staggerDelay?: number;
  maxDelay?: number;
}

const DEFAULT_STAGGER_DELAY = 50;
const DEFAULT_MAX_DELAY = 300;

/**
 * Staggers entrance animation across elements. Default: slide-up + fade-in.
 */
export function staggerEntrance(
  elements: Element[] | NodeListOf<Element>,
  animation?: (el: Element, i: number) => Animation,
  config?: StaggerConfig,
): Animation[] {
  const arr = Array.from(elements);
  const { staggerDelay = DEFAULT_STAGGER_DELAY, maxDelay = DEFAULT_MAX_DELAY } = config ?? {};

  const defaultAnimation = (el: Element, i: number) => {
    const delay = Math.min(i * staggerDelay, maxDelay);
    return el.animate(
      [
        { opacity: 0, transform: "translateY(12px)" },
        { opacity: 1, transform: "translateY(0)" },
      ],
      {
        duration: prefersReducedMotion() ? 0 : 200,
        delay: prefersReducedMotion() ? 0 : delay,
        easing: "cubic-bezier(0.16, 1, 0.3, 1)" /* --sc-ease-out */,
        fill: "forwards",
      },
    );
  };

  const anim = animation ?? defaultAnimation;
  return arr.map((el, i) => anim(el, i));
}

/** Simple fade-in. */
export function fadeIn(element: Element, duration = DEFAULT_DURATION): Animation {
  return withReducedMotion(
    () =>
      element.animate([{ opacity: 0 }, { opacity: 1 }], {
        duration,
        fill: "forwards",
      }),
    () =>
      element.animate([{ opacity: 0 }, { opacity: 1 }], {
        duration: 0,
        fill: "forwards",
      }),
  );
}

/** Simple fade-out. */
export function fadeOut(element: Element, duration = DEFAULT_DURATION): Animation {
  return withReducedMotion(
    () =>
      element.animate([{ opacity: 1 }, { opacity: 0 }], {
        duration,
        fill: "forwards",
      }),
    () =>
      element.animate([{ opacity: 1 }, { opacity: 0 }], {
        duration: 0,
        fill: "forwards",
      }),
  );
}

/** Slide up + fade in. */
export function slideUp(element: Element, distance = 12, duration = DEFAULT_DURATION): Animation {
  return withReducedMotion(
    () =>
      element.animate(
        [
          { opacity: 0, transform: `translateY(${distance}px)` },
          { opacity: 1, transform: "translateY(0)" },
        ],
        { duration, fill: "forwards", easing: "ease-out" },
      ),
    () =>
      element.animate(
        [
          { opacity: 0, transform: `translateY(${distance}px)` },
          { opacity: 1, transform: "translateY(0)" },
        ],
        { duration: 0, fill: "forwards" },
      ),
  );
}

/** Slide down + fade in. */
export function slideDown(element: Element, distance = 12, duration = DEFAULT_DURATION): Animation {
  return withReducedMotion(
    () =>
      element.animate(
        [
          { opacity: 0, transform: `translateY(-${distance}px)` },
          { opacity: 1, transform: "translateY(0)" },
        ],
        { duration, fill: "forwards", easing: "ease-out" },
      ),
    () =>
      element.animate(
        [
          { opacity: 0, transform: `translateY(-${distance}px)` },
          { opacity: 1, transform: "translateY(0)" },
        ],
        { duration: 0, fill: "forwards" },
      ),
  );
}

/** Scale in + fade in. */
export function scaleIn(element: Element, from = 0.95, duration = DEFAULT_DURATION): Animation {
  return withReducedMotion(
    () =>
      element.animate(
        [
          { opacity: 0, transform: `scale(${from})` },
          { opacity: 1, transform: "scale(1)" },
        ],
        { duration, fill: "forwards", easing: "ease-out" },
      ),
    () =>
      element.animate(
        [
          { opacity: 0, transform: `scale(${from})` },
          { opacity: 1, transform: "scale(1)" },
        ],
        { duration: 0, fill: "forwards" },
      ),
  );
}
