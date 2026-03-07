#!/usr/bin/env node
/**
 * Dynamic Color Extraction — Material 3-style "color from source"
 *
 * Takes a brand hex color and generates a harmonious oklch palette using
 * hue rotation and lightness shifting. Outputs JSON, CSS, or both.
 *
 * Usage:
 *   npx tsx design-tokens/dynamic-color.ts --hex "#7AB648"
 *   npx tsx design-tokens/dynamic-color.ts --hex "#ff6b35" --format css
 *   npx tsx design-tokens/dynamic-color.ts --hex "#7AB648" --format json
 */

interface OklchColor {
  l: number; // lightness 0-1
  c: number; // chroma 0-0.4
  h: number; // hue 0-360
}

interface ColorPalette {
  source: string;
  oklch: OklchColor;
  primary: Record<string, string>;
  secondary: Record<string, string>;
  tertiary: Record<string, string>;
  neutral: Record<string, string>;
  error: Record<string, string>;
}

function hexToRgb(hex: string): [number, number, number] {
  const m = hex.replace("#", "").match(/.{2}/g);
  if (!m) throw new Error(`Invalid hex: ${hex}`);
  return [
    parseInt(m[0], 16) / 255,
    parseInt(m[1], 16) / 255,
    parseInt(m[2], 16) / 255,
  ];
}

function rgbToLinear(c: number): number {
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}

function linearToRgb(c: number): number {
  return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
}

function rgbToOklch(r: number, g: number, b: number): OklchColor {
  const lr = rgbToLinear(r);
  const lg = rgbToLinear(g);
  const lb = rgbToLinear(b);

  const l_ = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
  const m_ = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
  const s_ = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb;

  const l_c = Math.cbrt(l_);
  const m_c = Math.cbrt(m_);
  const s_c = Math.cbrt(s_);

  const L = 0.2104542553 * l_c + 0.793617785 * m_c - 0.0040720468 * s_c;
  const a = 1.9779984951 * l_c - 2.428592205 * m_c + 0.4505937099 * s_c;
  const bv = 0.0259040371 * l_c + 0.7827717662 * m_c - 0.808675766 * s_c;

  const C = Math.sqrt(a * a + bv * bv);
  let h = (Math.atan2(bv, a) * 180) / Math.PI;
  if (h < 0) h += 360;

  return { l: L, c: C, h };
}

function oklchToRgb(L: number, C: number, h: number): [number, number, number] {
  const hRad = (h * Math.PI) / 180;
  const a = C * Math.cos(hRad);
  const b = C * Math.sin(hRad);

  const l_c = L + 0.3963377774 * a + 0.2158037573 * b;
  const m_c = L - 0.1055613458 * a - 0.0638541728 * b;
  const s_c = L - 0.0894841775 * a - 1.291485548 * b;

  const l_ = l_c * l_c * l_c;
  const m_ = m_c * m_c * m_c;
  const s_ = s_c * s_c * s_c;

  const r = +4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_;
  const g = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_;
  const bv = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.707614701 * s_;

  return [
    Math.max(0, Math.min(1, linearToRgb(r))),
    Math.max(0, Math.min(1, linearToRgb(g))),
    Math.max(0, Math.min(1, linearToRgb(bv))),
  ];
}

function rgbToHex(r: number, g: number, b: number): string {
  const toHex = (c: number) =>
    Math.round(c * 255)
      .toString(16)
      .padStart(2, "0");
  return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

function toP3(r: number, g: number, b: number): string {
  return `color(display-p3 ${r.toFixed(3)} ${g.toFixed(3)} ${b.toFixed(3)})`;
}

function generateScale(
  base: OklchColor,
  steps: number[],
): Record<string, string> {
  const result: Record<string, string> = {};
  for (const step of steps) {
    const lightness = step / 1000;
    const chroma = base.c * (1 - Math.abs(lightness - 0.5) * 0.4);
    const [r, g, b] = oklchToRgb(lightness, chroma, base.h);
    result[`${step}`] = rgbToHex(r, g, b);
    result[`${step}-p3`] = toP3(r, g, b);
  }
  return result;
}

function generatePalette(sourceHex: string): ColorPalette {
  const [r, g, b] = hexToRgb(sourceHex);
  const oklch = rgbToOklch(r, g, b);

  const steps = [50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 950];

  const primary = generateScale(oklch, steps);
  const secondary = generateScale(
    { l: oklch.l, c: oklch.c * 0.6, h: (oklch.h + 60) % 360 },
    steps,
  );
  const tertiary = generateScale(
    { l: oklch.l, c: oklch.c * 0.5, h: (oklch.h + 180) % 360 },
    steps,
  );

  const neutral = generateScale(
    { l: oklch.l, c: oklch.c * 0.05, h: oklch.h },
    steps,
  );

  const errorHue = 25;
  const error = generateScale({ l: oklch.l, c: 0.2, h: errorHue }, steps);

  return {
    source: sourceHex,
    oklch,
    primary,
    secondary,
    tertiary,
    neutral,
    error,
  };
}

function toCSS(palette: ColorPalette): string {
  const lines = [
    `/* Dynamic color palette generated from ${palette.source} */`,
    `/* oklch(${palette.oklch.l.toFixed(3)}, ${palette.oklch.c.toFixed(3)}, ${palette.oklch.h.toFixed(1)}) */`,
    "",
    ":root {",
  ];

  for (const [group, scale] of Object.entries({
    primary: palette.primary,
    secondary: palette.secondary,
    tertiary: palette.tertiary,
    neutral: palette.neutral,
    error: palette.error,
  })) {
    lines.push(`  /* ${group} */`);
    for (const [step, value] of Object.entries(scale)) {
      if (!step.endsWith("-p3")) {
        lines.push(`  --sc-dynamic-${group}-${step}: ${value};`);
      }
    }
  }

  lines.push("}");
  lines.push("");
  lines.push("@media (color-gamut: p3) {");
  lines.push("  :root {");

  for (const [group, scale] of Object.entries({
    primary: palette.primary,
    secondary: palette.secondary,
    tertiary: palette.tertiary,
    neutral: palette.neutral,
    error: palette.error,
  })) {
    for (const [step, value] of Object.entries(scale)) {
      if (step.endsWith("-p3")) {
        const baseStep = step.replace("-p3", "");
        lines.push(`    --sc-dynamic-${group}-${baseStep}: ${value};`);
      }
    }
  }

  lines.push("  }");
  lines.push("}");

  return lines.join("\n");
}

function main() {
  const args = process.argv.slice(2);
  let hex = "#7AB648";
  let format = "both";

  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--hex" && args[i + 1]) hex = args[++i];
    if (args[i] === "--format" && args[i + 1]) format = args[++i];
  }

  if (!hex.startsWith("#") || (hex.length !== 7 && hex.length !== 4)) {
    console.error("Invalid hex color. Use format: #rrggbb");
    process.exit(1);
  }

  const palette = generatePalette(hex);

  if (format === "json" || format === "both") {
    console.log(JSON.stringify(palette, null, 2));
  }
  if (format === "css" || format === "both") {
    if (format === "both") console.log("\n---\n");
    console.log(toCSS(palette));
  }
}

main();
