/**
 * Native (Swift/Kotlin/C) color emitters. Anything these cannot represent is
 * refused: the previous fall-through returned black, so an unsupported value
 * (e.g. oklch) shipped black to every native app with a green build.
 *
 * Colors are selected by `$type: "color"`, never by value spelling. Selecting
 * by spelling silently skipped any color written as `transparent`,
 * `color-mix()`, `var()` or an unresolved `{ref}`, because it did not look
 * like a color.
 */

import type { TokenMap, TypeMap } from "./token-lib.js";

const NATIVE_ACCEPTS = "#RRGGBB or rgb()/rgba()";

export class UnsupportedColorError extends Error {
  constructor(
    fn: string,
    value: string,
    tokenPath?: string,
    accepts = NATIVE_ACCEPTS,
  ) {
    const who = tokenPath ? `token ${tokenPath}: ` : "";
    super(
      `${fn}: ${who}unsupported color ${JSON.stringify(value)}. This emitter accepts ` +
        `${accepts}. Put web-only colors (oklch, display-p3) in a ` +
        `token file whose $extensions["com.human.platform"]: "web".`,
    );
    this.name = "UnsupportedColorError";
  }
}

const HEX6 = /^#([0-9a-fA-F]{6})$/;
const RGBA = /rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/;
const RGBA_EXACT = /^rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*(\d*\.?\d+))?\)$/;

/** rgb()/rgba() with channels 0–255 and alpha 0–1; anything else would emit out-of-range Swift/Kotlin values. */
function isNativeRGBA(v: string): boolean {
  const m = v.match(RGBA_EXACT);
  if (!m) return false;
  const channelsOk = [m[1], m[2], m[3]].every((c) => Number(c) <= 255);
  const alphaOk = m[4] === undefined || Number(m[4]) <= 1;
  return channelsOk && alphaOk;
}

/**
 * Every `$type: "color"` token whose path starts with `prefix`, sorted by
 * path, as [path, value]. A color-typed value the Swift/Kotlin converters
 * cannot represent throws UnsupportedColorError naming the token; tokens of
 * any other $type are skipped whatever their value looks like.
 */
export function nativeColorEntries(
  tokens: TokenMap,
  types: TypeMap,
  prefix: string,
): Array<[string, string]> {
  return Object.keys(tokens)
    .filter((k) => k.startsWith(prefix) && types[k] === "color")
    .sort()
    .map((k) => {
      const v = tokens[k];
      if (typeof v !== "string" || !(HEX6.test(v) || isNativeRGBA(v))) {
        throw new UnsupportedColorError("nativeColorEntries", String(v), k);
      }
      return [k, v];
    });
}

/** #RRGGBB → [r, g, b] for the C header's terminal escapes. No alpha there, so rgba() is refused too. */
export function terminalRGB(
  tokenPath: string,
  value: unknown,
): [number, number, number] {
  const m = typeof value === "string" ? value.match(HEX6) : null;
  if (!m) {
    throw new UnsupportedColorError("terminalRGB", String(value), tokenPath, "#RRGGBB");
  }
  return [
    parseInt(m[1].substring(0, 2), 16),
    parseInt(m[1].substring(2, 4), 16),
    parseInt(m[1].substring(4, 6), 16),
  ];
}

/** #rrggbb → 0xRRGGBB */
export function hexToSwift(hex: string): string {
  const m = hex.match(HEX6);
  if (!m) throw new UnsupportedColorError("hexToSwift", hex);
  return "0x" + m[1].toUpperCase();
}

/** #rrggbb → 0xFFRRGGBB */
export function hexToKotlin(hex: string): string {
  const m = hex.match(HEX6);
  if (!m) throw new UnsupportedColorError("hexToKotlin", hex);
  return "0xFF" + m[1].toUpperCase();
}

/** rgb[a](r,g,b[,a]) → 0xAARRGGBB */
export function rgbaToKotlin(rgba: string): string {
  const m = rgba.match(RGBA);
  if (!m) throw new UnsupportedColorError("rgbaToKotlin", rgba);
  const r = parseInt(m[1], 10);
  const g = parseInt(m[2], 10);
  const b = parseInt(m[3], 10);
  const a = m[4] ? Math.round(parseFloat(m[4]) * 255) : 255;
  const hex = (((a << 24) | (r << 16) | (g << 8) | b) >>> 0)
    .toString(16)
    .padStart(8, "0")
    .toUpperCase();
  return "0x" + hex;
}

export function colorToKotlin(val: string): string {
  if (val.startsWith("#")) return hexToKotlin(val);
  if (/^rgba?\(/.test(val)) return rgbaToKotlin(val);
  throw new UnsupportedColorError("colorToKotlin", val);
}

export function formatSwiftColor(val: string): string {
  if (val.startsWith("#")) return `Color(hex: ${hexToSwift(val)})`;
  const m = val.match(RGBA);
  if (m) {
    const r = Math.round((parseInt(m[1], 10) / 255) * 10000) / 10000;
    const g = Math.round((parseInt(m[2], 10) / 255) * 10000) / 10000;
    const b = Math.round((parseInt(m[3], 10) / 255) * 10000) / 10000;
    const a = m[4] ? Math.round(parseFloat(m[4]) * 10000) / 10000 : 1;
    return `Color(red: ${r}, green: ${g}, blue: ${b}, opacity: ${a})`;
  }
  throw new UnsupportedColorError("formatSwiftColor", val);
}
