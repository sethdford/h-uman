/**
 * Spring physics utilities for SeaClaw UI animations.
 * Simulates damped harmonic oscillator; aligns with motion tokens.
 */

export interface SpringConfig {
  stiffness: number;
  damping: number;
  mass: number;
  initialVelocity?: number;
}

export const SPRING_PRESETS = {
  micro: { stiffness: 400, damping: 30, mass: 1 },
  standard: { stiffness: 200, damping: 20, mass: 1 },
  expressive: { stiffness: 120, damping: 14, mass: 1 },
  dramatic: { stiffness: 80, damping: 10, mass: 1 },
} as const satisfies Record<string, SpringConfig>;

export type SpringPresetName = keyof typeof SPRING_PRESETS;

const DEFAULT_DT = 1 / 60;
const SETTLE_THRESHOLD = 0.001;
const MAX_SAMPLES = 120;

/**
 * Simulates spring differential equation to produce keyframe values (0–1 normalized).
 * Uses Euler integration for the damped harmonic oscillator.
 */
export function generateSpringKeyframes(config: SpringConfig): number[] {
  const { stiffness, damping, mass, initialVelocity = 0 } = config;
  const k = stiffness;
  const c = damping;
  const m = mass;

  let x = 0;
  let v = initialVelocity;
  const keyframes: number[] = [0];

  for (let i = 0; i < MAX_SAMPLES; i++) {
    const force = -k * (x - 1) - c * v;
    const a = force / m;
    v += a * DEFAULT_DT;
    x += v * DEFAULT_DT;

    keyframes.push(Math.max(0, x));

    if (Math.abs(x - 1) < SETTLE_THRESHOLD && Math.abs(v) < SETTLE_THRESHOLD) {
      keyframes.push(1);
      break;
    }
  }

  return keyframes;
}

/**
 * Converts spring keyframes to a CSS linear() easing string.
 * Format: linear(0, value1 time1%, value2 time2%, ..., 1)
 */
export function springToLinearEasing(config: SpringConfig): string {
  const keyframes = generateSpringKeyframes(config);
  if (keyframes.length <= 2) return "linear";

  const parts: string[] = ["0"];
  const lastIdx = keyframes.length - 1;

  for (let i = 1; i < lastIdx; i++) {
    const t = (i / lastIdx) * 100;
    const v = keyframes[i];
    parts.push(`${v} ${t.toFixed(1)}%`);
  }
  parts.push("1");

  return `linear(0, ${parts.join(", ")})`;
}

/**
 * Returns the CSS linear() string for a named spring preset.
 */
export function springEasing(preset: SpringPresetName): string {
  return springToLinearEasing(SPRING_PRESETS[preset]);
}
