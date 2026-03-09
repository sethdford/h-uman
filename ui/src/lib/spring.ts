/**
 * Lightweight spring physics engine for interactive UI animations.
 *
 * Uses a damped harmonic oscillator model. All animations run on
 * requestAnimationFrame and animate only compositor-friendly properties
 * (transform, opacity, filter) to maintain 60fps — never width, height,
 * top, left, margin, or padding (avoids layout thrashing).
 *
 * Frame budget guard: if a tick exceeds 16ms, snap to target and resolve.
 */

export interface SpringConfig {
  stiffness: number;
  damping: number;
  mass: number;
  precision?: number;
}

export const SPRING_PRESETS = {
  micro: { stiffness: 500, damping: 30, mass: 1, precision: 0.01 } as SpringConfig,
  standard: { stiffness: 200, damping: 20, mass: 1, precision: 0.01 } as SpringConfig,
  expressive: { stiffness: 180, damping: 14, mass: 1, precision: 0.005 } as SpringConfig,
  dramatic: { stiffness: 120, damping: 10, mass: 1, precision: 0.005 } as SpringConfig,
  gentle: { stiffness: 150, damping: 22, mass: 1, precision: 0.01 } as SpringConfig,
  bounce: { stiffness: 300, damping: 12, mass: 1, precision: 0.005 } as SpringConfig,
  snappy: { stiffness: 400, damping: 28, mass: 1, precision: 0.01 } as SpringConfig,
} as const;

interface SpringState {
  value: number;
  velocity: number;
}

function stepSpring(
  state: SpringState,
  target: number,
  config: SpringConfig,
  dt: number,
): SpringState {
  const { stiffness, damping, mass } = config;
  const displacement = state.value - target;
  const springForce = -stiffness * displacement;
  const dampingForce = -damping * state.velocity;
  const acceleration = (springForce + dampingForce) / mass;
  const velocity = state.velocity + acceleration * dt;
  const value = state.value + velocity * dt;
  return { value, velocity };
}

function isAtRest(state: SpringState, target: number, precision: number): boolean {
  return Math.abs(state.value - target) < precision && Math.abs(state.velocity) < precision;
}

export interface SpringAnimation {
  stop: () => void;
  promise: Promise<void>;
}

type AnimatableProperty = "scaleX" | "scaleY" | "translateX" | "translateY" | "opacity" | "rotate";

interface PropertyTarget {
  property: AnimatableProperty;
  from: number;
  to: number;
}

/**
 * Animate an element's transform/opacity properties with spring physics.
 * Returns a handle to stop the animation and a promise that resolves on completion.
 */
export function springAnimate(
  element: HTMLElement,
  targets: PropertyTarget[],
  config: SpringConfig = SPRING_PRESETS.standard,
): SpringAnimation {
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  if (reducedMotion) {
    applyProperties(
      element,
      targets.map((t) => ({ ...t, current: t.to })),
    );
    return { stop: () => {}, promise: Promise.resolve() };
  }

  const states: SpringState[] = targets.map((t) => ({ value: t.from, velocity: 0 }));
  let rafId = 0;
  let stopped = false;
  let lastTime = 0;

  const FRAME_BUDGET_MS = 16;

  const promise = new Promise<void>((resolve) => {
    function tick(now: number) {
      if (stopped) {
        resolve();
        return;
      }
      const tickStart = performance.now();
      const dt = lastTime === 0 ? 1 / 60 : Math.min((now - lastTime) / 1000, 1 / 30);
      lastTime = now;

      let allAtRest = true;
      const precision = config.precision ?? 0.01;

      for (let i = 0; i < states.length; i++) {
        states[i] = stepSpring(states[i], targets[i].to, config, dt);
        if (!isAtRest(states[i], targets[i].to, precision)) {
          allAtRest = false;
        }
      }

      const elapsed = performance.now() - tickStart;
      if (elapsed > FRAME_BUDGET_MS) {
        applyProperties(
          element,
          targets.map((t) => ({ ...t, current: t.to })),
        );
        resolve();
        return;
      }

      applyProperties(
        element,
        targets.map((t, i) => ({ ...t, current: states[i].value })),
      );

      if (allAtRest) {
        applyProperties(
          element,
          targets.map((t) => ({ ...t, current: t.to })),
        );
        resolve();
      } else {
        rafId = requestAnimationFrame(tick);
      }
    }

    rafId = requestAnimationFrame(tick);
  });

  return {
    stop: () => {
      stopped = true;
      if (rafId) cancelAnimationFrame(rafId);
    },
    promise,
  };
}

interface PropertyWithCurrent extends PropertyTarget {
  current: number;
}

function applyProperties(element: HTMLElement, props: PropertyWithCurrent[]): void {
  const transforms: string[] = [];
  let hasOpacity = false;
  let opacityValue = 1;

  for (const p of props) {
    switch (p.property) {
      case "scaleX":
        transforms.push(`scaleX(${p.current})`);
        break;
      case "scaleY":
        transforms.push(`scaleY(${p.current})`);
        break;
      case "translateX":
        transforms.push(`translateX(${p.current}px)`);
        break;
      case "translateY":
        transforms.push(`translateY(${p.current}px)`);
        break;
      case "rotate":
        transforms.push(`rotate(${p.current}deg)`);
        break;
      case "opacity":
        hasOpacity = true;
        opacityValue = p.current;
        break;
    }
  }

  if (transforms.length) {
    element.style.transform = transforms.join(" ");
  }
  if (hasOpacity) {
    element.style.opacity = String(opacityValue);
  }
}

/**
 * Shorthand: spring scale from -> to on both axes.
 */
export function springScale(
  element: HTMLElement,
  from: number,
  to: number,
  config: SpringConfig = SPRING_PRESETS.expressive,
): SpringAnimation {
  return springAnimate(
    element,
    [
      { property: "scaleX", from, to },
      { property: "scaleY", from, to },
    ],
    config,
  );
}

/**
 * Shorthand: spring entry from below with scale + opacity.
 */
export function springEntry(
  element: HTMLElement,
  translateY = 20,
  config: SpringConfig = SPRING_PRESETS.expressive,
): SpringAnimation {
  return springAnimate(
    element,
    [
      { property: "translateY", from: translateY, to: 0 },
      { property: "scaleX", from: 0.95, to: 1 },
      { property: "scaleY", from: 0.95, to: 1 },
      { property: "opacity", from: 0, to: 1 },
    ],
    config,
  );
}

/**
 * Shorthand: spring "squash and stretch" press effect.
 * Compresses then bounces back.
 */
export function springPress(
  element: HTMLElement,
  config: SpringConfig = SPRING_PRESETS.micro,
): SpringAnimation {
  return springAnimate(
    element,
    [
      { property: "scaleX", from: 1, to: 0.96 },
      { property: "scaleY", from: 1, to: 0.96 },
    ],
    config,
  );
}

/**
 * Shorthand: spring release after press (back to full scale).
 */
export function springRelease(
  element: HTMLElement,
  config: SpringConfig = SPRING_PRESETS.bounce,
): SpringAnimation {
  return springAnimate(
    element,
    [
      { property: "scaleX", from: 0.96, to: 1 },
      { property: "scaleY", from: 0.96, to: 1 },
    ],
    config,
  );
}
