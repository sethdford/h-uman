/**
 * Contrast measurement for the Quiet Room layer (spec II.3 / II.7-A4).
 * WCAG 2.x relative luminance; OKLCH → linear sRGB via Björn Ottosson's
 * published matrices. Anything that cannot be measured as an opaque sRGB
 * color is refused — a guessed ratio is worse than none.
 */
import type { TokenMap } from "./token-lib.js";

export type Linear = readonly [number, number, number];

export class UnmeasurableColorError extends Error {
  constructor(value: string, why: string) {
    super(`cannot measure ${JSON.stringify(value)}: ${why}`);
    this.name = "UnmeasurableColorError";
  }
}

const HEX6 = /^#([0-9a-fA-F]{6})$/;
const RGB =
  /^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([\d.]+)\s*)?\)$/;
const OKLCH = /^oklch\(\s*([\d.]+)(%?)\s+([\d.]+)\s+([\d.]+)\s*\)$/;

const toLinear = (c: number) =>
  c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;

export function oklchToLinear(L: number, C: number, h: number): Linear {
  const a = C * Math.cos((h * Math.PI) / 180);
  const b = C * Math.sin((h * Math.PI) / 180);
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ];
}

export function parseOpaqueColor(v: string): Linear {
  const s = v.trim();
  const hex = s.match(HEX6);
  if (hex) {
    const n = hex[1];
    return [0, 2, 4].map((i) => toLinear(parseInt(n.slice(i, i + 2), 16) / 255)) as unknown as Linear;
  }
  const rgb = s.match(RGB);
  if (rgb) {
    if (rgb[4] !== undefined && parseFloat(rgb[4]) < 1) {
      throw new UnmeasurableColorError(v, "translucent; contrast depends on what is behind it");
    }
    return [1, 2, 3].map((i) => toLinear(parseInt(rgb[i], 10) / 255)) as unknown as Linear;
  }
  const ok = s.match(OKLCH);
  if (ok) {
    const L = ok[2] === "%" ? parseFloat(ok[1]) / 100 : parseFloat(ok[1]);
    return oklchToLinear(L, parseFloat(ok[3]), parseFloat(ok[4]));
  }
  throw new UnmeasurableColorError(v, "not #RRGGBB, opaque rgb(), or oklch()");
}

export function isInGamut(lin: Linear, eps = 1e-4): boolean {
  return lin.every((c) => c >= -eps && c <= 1 + eps);
}

function luminance(lin: Linear): number {
  const [r, g, b] = lin.map((c) => Math.min(Math.max(c, 0), 1));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastRatio(fg: string, bg: string): number {
  const a = luminance(parseOpaqueColor(fg));
  const b = luminance(parseOpaqueColor(bg));
  return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
}

/** Backgrounds every text/UI role must read on (spec II.3). */
export const BG_ROLES = [
  "bg", "bg-inset", "bg-surface", "bg-elevated", "surface-container",
  "surface-container-high", "surface-container-highest", "bg-overlay",
] as const;
/** Roles rendered as text: ≥ 4.5:1 on every background. */
export const TEXT_ROLES = [
  "text", "text-secondary", "text-muted", "text-tertiary", "text-faint",
  "accent-text", "link", "link-hover", "link-active", "link-visited",
  "success", "warning", "error", "info",
] as const;
/** Non-text UI indicators: ≥ 3:1 on every background. */
export const UI_ROLES = ["focus-ring"] as const;
/** [label, fill] pairs: the label must read on the fill at ≥ 4.5:1. */
export const FILL_PAIRS = [
  ["on-accent", "accent"],
  ["on-accent", "accent-hover"],
] as const;

export interface PairResult {
  mode: "light" | "dark";
  fg: string;
  bg: string;
  need: number;
  ratio: number | null;
  ok: boolean;
  reason?: string;
}

/**
 * Effective value = the quiet override if present, else the inherited
 * semantic value for that mode. Inherited tokens are measured too: they
 * render on paper whether or not the quiet file mentions them.
 */
export function checkQuietContrast(shared: TokenMap, web: TokenMap): PairResult[] {
  const results: PairResult[] = [];
  for (const mode of ["light", "dark"] as const) {
    const eff = (n: string) => {
      const v = web[`quiet.${mode}.${n}`] ?? shared[`${mode}.${n}`];
      return v === undefined ? undefined : String(v);
    };
    const measure = (fg: string, bg: string, need: number) => {
      const f = eff(fg);
      const b = eff(bg);
      if (f === undefined || b === undefined) {
        results.push({ mode, fg, bg, need, ratio: null, ok: false, reason: "missing" });
        return;
      }
      try {
        const ratio = contrastRatio(f, b);
        results.push({ mode, fg, bg, need, ratio, ok: ratio >= need });
      } catch (e) {
        results.push({ mode, fg, bg, need, ratio: null, ok: false, reason: (e as Error).message });
      }
    };
    for (const bg of BG_ROLES) {
      for (const fg of TEXT_ROLES) measure(fg, bg, 4.5);
      for (const fg of UI_ROLES) measure(fg, bg, 3);
    }
    for (const [label, fill] of FILL_PAIRS) measure(label, fill, 4.5);
  }
  return results;
}

/** Every oklch() value in the web layer must be inside sRGB. */
export function checkQuietGamut(web: TokenMap): string[] {
  const errors: string[] = [];
  for (const [k, v] of Object.entries(web)) {
    const s = String(v);
    if (!s.startsWith("oklch(")) continue;
    if (!isInGamut(parseOpaqueColor(s))) errors.push(`${k} = ${s} is outside the sRGB gamut`);
  }
  return errors;
}
