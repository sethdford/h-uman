/**
 * Native (Swift/Kotlin) color emitters. Anything these cannot represent is
 * refused: the previous fall-through returned black, so an unsupported value
 * (e.g. oklch) shipped black to every native app with a green build.
 */

export class UnsupportedColorError extends Error {
  constructor(fn: string, value: string) {
    super(
      `${fn}: unsupported color ${JSON.stringify(value)}. Native emitters accept ` +
        `#RRGGBB or rgb()/rgba(). Put web-only colors (oklch, display-p3) in a ` +
        `token file with "com.human.platform": "web".`,
    );
    this.name = "UnsupportedColorError";
  }
}

const HEX6 = /^#([0-9a-fA-F]{6})$/;
const RGBA = /rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/;

/** True for values written in a CSS color syntax (supported by native emitters or not). Non-colors — dimensions, angles, percentages, gradients, numbers — are false. */
export function isColorLike(v: string): boolean {
  return /^(#|rgba?\(|hsla?\(|hwb\(|lab\(|lch\(|oklab\(|oklch\(|color\()/i.test(
    v.trim(),
  );
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
