#!/usr/bin/env node
/**
 * Design tokens build pipeline
 * Reads W3C-format token JSON files and generates platform-specific outputs.
 * Run: npx tsx design-tokens/build.ts
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";
import { execFileSync } from "child_process";

const REM_PX = 16;
const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, "..");
const TOKENS_DIR = path.join(ROOT, "design-tokens");

const TOKEN_FILES = [
  "base.tokens.json",
  "typography.tokens.json",
  "motion.tokens.json",
  "semantic.tokens.json",
  "components.tokens.json",
  "opacity.tokens.json",
  "elevation.tokens.json",
  "breakpoints.tokens.json",
];

type TokenValue = string | number;
type TokenMap = Record<string, TokenValue>;

/** Recursively collect all $value entries into a flat path -> value map */
function collectTokens(obj: unknown, prefix = ""): TokenMap {
  const result: TokenMap = {};
  if (obj === null || typeof obj !== "object") return result;
  const rec = obj as Record<string, unknown>;

  for (const [key, val] of Object.entries(rec)) {
    if (key.startsWith("$")) continue;
    const pathPart = prefix ? `${prefix}.${key}` : key;
    if (val !== null && typeof val === "object" && "$value" in val) {
      const v = (val as { $value: TokenValue }).$value;
      result[pathPart] = v;
    } else if (typeof val === "object" && val !== null) {
      Object.assign(result, collectTokens(val, pathPart));
    }
  }
  return result;
}

/** Resolve {path.to.token} references in place; repeat until stable */
function resolveRefs(tokens: TokenMap): TokenMap {
  const resolved = { ...tokens };
  let changed = true;
  while (changed) {
    changed = false;
    for (const [key, val] of Object.entries(resolved)) {
      if (typeof val !== "string") continue;
      const ref = val.match(/^\{([^}]+)\}$/);
      if (ref) {
        const target = resolved[ref[1]];
        if (target !== undefined) {
          resolved[key] = target;
          changed = true;
        }
      }
    }
  }
  return resolved;
}

/** Convert rem to px (1rem = 16px). Returns number for px, string unchanged if not rem. */
function remToPx(val: string): number | null {
  const m = val.match(/^([\d.]+)rem$/);
  if (!m) return null;
  return Math.round(parseFloat(m[1]) * REM_PX);
}

/** Parse dimension (rem, px) to numeric px */
function dimToPx(val: string): number | null {
  if (val.endsWith("rem")) return remToPx(val);
  const m = val.match(/^(\d+)px$/);
  return m ? parseInt(m[1], 10) : null;
}

/** Parse duration (ms, s) to milliseconds */
function parseDurationMs(val: string): number | null {
  const mMs = val.match(/^(\d+)ms$/);
  if (mMs) return parseInt(mMs[1], 10);
  const mS = val.match(/^([\d.]+)s$/);
  if (mS) return Math.round(parseFloat(mS[1]) * 1000);
  return null;
}

/** Parse em value (e.g. "0.02em", "-0.01em") to number */
function parseEmValue(val: string): number | null {
  const m = val.match(/^(-?[\d.]+)em$/);
  return m ? parseFloat(m[1]) : null;
}

/** Convert hex #rrggbb to ANSI 256-color code (6x6x6 cube, indices 16-231) */
function hexToAnsi256(hex: string): number {
  const m = hex.match(/^#([0-9a-fA-F]{6})$/);
  if (!m) return 7;
  const r = parseInt(m[1].substring(0, 2), 16);
  const g = parseInt(m[1].substring(2, 4), 16);
  const b = parseInt(m[1].substring(4, 6), 16);
  const ri = Math.round((r / 255) * 5);
  const gi = Math.round((g / 255) * 5);
  const bi = Math.round((b / 255) * 5);
  return 16 + 36 * ri + 6 * gi + bi;
}

/** Convert hex color #rrggbb to 0xRRGGBB for Swift */
function hexToSwift(hex: string): string {
  const m = hex.match(/^#([0-9a-fA-F]{6})$/);
  if (!m) return "0x000000";
  return "0x" + m[1].toUpperCase();
}

/** Convert hex color to Kotlin Color(0xFFRRGGBB) */
function hexToKotlin(hex: string): string {
  const m = hex.match(/^#([0-9a-fA-F]{6})$/);
  if (!m) return "0xFF000000";
  return "0xFF" + m[1].toUpperCase();
}

/** Convert rgba(r,g,b,a) to Kotlin Color - approximate as opaque for simplicity */
function rgbaToKotlin(rgba: string): string {
  const m = rgba.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/);
  if (!m) return "0xFF000000";
  const r = parseInt(m[1], 10);
  const g = parseInt(m[2], 10);
  const b = parseInt(m[3], 10);
  const a = m[4] ? Math.round(parseFloat(m[4]) * 255) : 255;
  const hex = ((a << 24) | (r << 16) | (g << 8) | b)
    .toString(16)
    .padStart(8, "0")
    .toUpperCase();
  return "0x" + hex;
}

function colorToSwift(val: string): string {
  if (val.startsWith("#")) return hexToSwift(val);
  if (val.startsWith("rgba")) return hexToSwift("#000000"); // Swift Color(hex:) doesn't support alpha directly; use placeholder
  return "0x000000";
}

function colorToKotlin(val: string): string {
  if (val.startsWith("#")) return hexToKotlin(val);
  if (val.startsWith("rgba")) return rgbaToKotlin(val);
  return "0xFF000000";
}

/** k=stiffness, c=damping, m=mass. SwiftUI: response ≈ 2π/√(k/m), dampingFraction = c/(2√(km)) */
function springToSwiftResponse(
  stiffness: number,
  damping: number,
  mass: number,
): number {
  return (
    Math.round(((2 * Math.PI) / Math.sqrt(stiffness / mass)) * 1000) / 1000
  );
}

function springToSwiftDampingFraction(
  stiffness: number,
  damping: number,
  mass: number,
): number {
  return (
    Math.round((damping / (2 * Math.sqrt(stiffness * mass))) * 1000) / 1000
  );
}

function parseOutdir(): string | null {
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--outdir" && args[i + 1]) return args[i + 1];
    if (args[i].startsWith("--outdir=")) return args[i].split("=")[1];
  }
  return null;
}

function writeOutput(
  outdir: string | null,
  defaultPath: string,
  flatName: string,
  content: string,
) {
  const dest = outdir ? path.join(outdir, flatName) : defaultPath;
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.writeFileSync(dest, content, "utf-8");
  console.log("Wrote", dest);
}

function main() {
  let tokens: TokenMap = {};
  for (const file of TOKEN_FILES) {
    const p = path.join(TOKENS_DIR, file);
    if (!fs.existsSync(p)) {
      console.error(`Missing token file: ${p}`);
      process.exit(1);
    }
    const data = JSON.parse(fs.readFileSync(p, "utf-8"));
    tokens = { ...tokens, ...collectTokens(data) };
  }
  tokens = resolveRefs(tokens);

  const outdir = parseOutdir();

  const css = generateCSS(tokens);
  writeOutput(
    outdir,
    path.join(ROOT, "ui", "src", "styles", "_tokens.css"),
    "_tokens.css",
    css,
  );
  if (!outdir) {
    writeOutput(
      null,
      path.join(ROOT, "website", "src", "styles", "_tokens.css"),
      "_tokens.css",
      css,
    );
  }

  const swift = generateSwift(tokens);
  writeOutput(
    outdir,
    path.join(
      ROOT,
      "apps",
      "shared",
      "SeaClawKit",
      "Sources",
      "SeaClawChatUI",
      "DesignTokens.swift",
    ),
    "DesignTokens.swift",
    swift,
  );

  const kotlin = generateKotlin(tokens);
  writeOutput(
    outdir,
    path.join(
      ROOT,
      "apps",
      "android",
      "app",
      "src",
      "main",
      "java",
      "ai",
      "seaclaw",
      "app",
      "ui",
      "DesignTokens.kt",
    ),
    "DesignTokens.kt",
    kotlin,
  );

  const header = generateCHeader(tokens);
  const headerPath = outdir
    ? path.join(outdir, "design_tokens.h")
    : path.join(ROOT, "include", "seaclaw", "design_tokens.h");
  writeOutput(outdir, headerPath, "design_tokens.h", header);

  try {
    execFileSync("clang-format", ["-i", headerPath], { stdio: "ignore" });
  } catch {
    try {
      execFileSync(
        "/opt/homebrew/opt/llvm/bin/clang-format",
        ["-i", headerPath],
        {
          stdio: "ignore",
        },
      );
    } catch {
      /* clang-format not available — skip */
    }
  }

  console.log("Done.");
}

const COMPONENT_PREFIXES = [
  "sidebar",
  "button",
  "card",
  "modal",
  "badge",
  "toast",
  "input",
  "tooltip",
  "dropdown",
  "tabs",
  "avatar",
  "progress",
  "sheet",
  "command-palette",
  "floating-action-button",
];

function generateCSS(tokens: TokenMap): string {
  const lines: string[] = [
    "/* Auto-generated from design-tokens/ — do not edit manually */",
    ":root {",
  ];

  // Base: Spacing — ALL spacing keys dynamically
  lines.push("  /* Base: Spacing */");
  const spacingKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("spacing."))
    .sort();
  for (const k of spacingKeys) {
    const v = tokens[k];
    if (v != null)
      lines.push(`  --sc-space-${k.replace("spacing.", "")}: ${v};`);
  }

  // Base: Radius
  lines.push("  /* Base: Radius */");
  lines.push(`  --sc-radius: ${tokens["radius.md"] ?? "8px"};`);
  const radiusKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("radius."))
    .sort();
  for (const k of radiusKeys) {
    const v = tokens[k];
    if (v != null)
      lines.push(`  --sc-radius-${k.replace("radius.", "")}: ${v};`);
  }

  // Z-index
  lines.push("  /* Z-index */");
  const zIndexKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("z-index."))
    .sort();
  for (const k of zIndexKeys) {
    const v = tokens[k];
    if (v != null) lines.push(`  --sc-z-${k.replace("z-index.", "")}: ${v};`);
  }

  // Opacity
  lines.push("  /* Opacity */");
  const opacityKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("opacity."))
    .sort();
  for (const k of opacityKeys) {
    const v = tokens[k];
    if (v != null)
      lines.push(`  --sc-opacity-${k.replace("opacity.", "")}: ${v};`);
  }

  // Elevation (skip elevation-role)
  lines.push("  /* Elevation */");
  const elevationKeys = Object.keys(tokens)
    .filter(
      (k) => k.startsWith("elevation.") && !k.startsWith("elevation-role."),
    )
    .sort();
  for (const k of elevationKeys) {
    const v = tokens[k];
    if (v != null) {
      const suffix = k.replace("elevation.", "");
      if (suffix && !suffix.includes("."))
        lines.push(`  --sc-elevation-${suffix}: ${v};`);
    }
  }

  // Breakpoints
  lines.push("  /* Breakpoints */");
  const breakpointKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("breakpoint."))
    .sort();
  for (const k of breakpointKeys) {
    const v = tokens[k];
    if (v != null)
      lines.push(`  --sc-breakpoint-${k.replace("breakpoint.", "")}: ${v};`);
  }

  // Typography
  lines.push("  /* Typography */");
  lines.push(
    `  --sc-font: ${tokens["fontFamily.sans"] ?? "'Avenir', sans-serif"};`,
  );
  lines.push(
    `  --sc-font-mono: ${tokens["fontFamily.mono"] ?? "'Geist Mono', monospace"};`,
  );
  for (const [k, v] of Object.entries(tokens)) {
    if (k.startsWith("fontSize.") && typeof v === "string")
      lines.push(`  --sc-text-${k.replace("fontSize.", "")}: ${v};`);
    if (k.startsWith("fontWeight.") && typeof v === "number")
      lines.push(`  --sc-weight-${k.replace("fontWeight.", "")}: ${v};`);
    if (k.startsWith("lineHeight.") && typeof v === "number")
      lines.push(`  --sc-leading-${k.replace("lineHeight.", "")}: ${v};`);
    if (k.startsWith("letterSpacing.") && typeof v === "string")
      lines.push(`  --sc-tracking-${k.replace("letterSpacing.", "")}: ${v};`);
  }

  // Type roles
  lines.push("  /* Type roles */");
  const typeRoleKeys = Object.keys(tokens).filter(
    (k) => k.startsWith("typeRole.") && k.split(".").length >= 3,
  );
  const typeRoleGroups = new Map<string, string[]>();
  for (const k of typeRoleKeys) {
    const parts = k.split(".");
    const role = parts[1];
    const prop = parts[2];
    if (!typeRoleGroups.has(role)) typeRoleGroups.set(role, []);
    typeRoleGroups.get(role)!.push(prop);
  }
  const propToCss = (p: string) =>
    p
      .replace(/([A-Z])/g, "-$1")
      .toLowerCase()
      .replace(/^-/, "");
  for (const [role, props] of [...typeRoleGroups.entries()].sort()) {
    for (const prop of props.sort()) {
      const v = tokens[`typeRole.${role}.${prop}`];
      if (v != null) {
        const cssProp = propToCss(prop);
        const suffix =
          cssProp === "font-size"
            ? "size"
            : cssProp === "font-weight"
              ? "weight"
              : cssProp === "line-height"
                ? "line-height"
                : cssProp === "letter-spacing"
                  ? "letter-spacing"
                  : cssProp;
        lines.push(`  --sc-type-${role.replace(/-/g, "-")}-${suffix}: ${v};`);
      }
    }
  }

  // Focus ring tokens (from dark theme)
  lines.push("  /* Focus ring */");
  if (tokens["dark.focus-ring"] != null)
    lines.push(`  --sc-focus-ring: ${tokens["dark.focus-ring"]};`);
  if (tokens["dark.focus-ring-width"] != null)
    lines.push(`  --sc-focus-ring-width: ${tokens["dark.focus-ring-width"]};`);
  if (tokens["dark.focus-ring-offset"] != null)
    lines.push(
      `  --sc-focus-ring-offset: ${tokens["dark.focus-ring-offset"]};`,
    );

  // Choreography tokens
  lines.push("  /* Choreography */");
  const choreographyKeys = [
    "choreography.stagger-delay",
    "choreography.stagger-max",
    "choreography.cascade-delay",
    "choreography.cascade-max",
  ];
  for (const k of choreographyKeys) {
    const v = tokens[k];
    if (v != null) {
      const name = k.replace("choreography.", "").replace(/-/g, "-");
      lines.push(`  --sc-${name}: ${v};`);
    }
  }

  // Purpose-based motion
  lines.push("  /* Purpose-based motion */");
  const purposeKeys = Object.keys(tokens).filter(
    (k) => k.startsWith("purpose.") && k.split(".").length >= 3,
  );
  const purposeGroups = new Map<string, string[]>();
  for (const k of purposeKeys) {
    const parts = k.split(".");
    const purpose = parts[1];
    const prop = parts[2];
    if (!purposeGroups.has(purpose)) purposeGroups.set(purpose, []);
    purposeGroups.get(purpose)!.push(prop);
  }
  for (const [purpose, props] of [...purposeGroups.entries()].sort()) {
    for (const prop of props.sort()) {
      const v = tokens[`purpose.${purpose}.${prop}`];
      if (v != null) {
        const suffix = purpose.replace(/-/g, "-") + "-" + prop;
        lines.push(`  --sc-${suffix}: ${v};`);
      }
    }
  }

  // Motion
  lines.push("  /* Motion */");
  for (const [k, v] of Object.entries(tokens)) {
    if (k.startsWith("duration.") && typeof v === "string")
      lines.push(`  --sc-duration-${k.replace("duration.", "")}: ${v};`);
    if (k.startsWith("easing.") && typeof v === "string")
      lines.push(`  --sc-${k.replace("easing.", "")}: ${v};`);
    if (k.startsWith("transition.") && typeof v === "string")
      lines.push(`  --sc-transition: ${v};`);
  }
  for (const [k, v] of Object.entries(tokens)) {
    if (k.match(/^spring\.\w+\.stiffness$/)) {
      const name = k.split(".")[1];
      const stiff = v as number;
      const damp = (tokens[`spring.${name}.damping`] as number) ?? 20;
      const mass = (tokens[`spring.${name}.mass`] as number) ?? 1;
      lines.push(`  --sc-spring-${name}-stiffness: ${stiff};`);
      lines.push(`  --sc-spring-${name}-damping: ${damp};`);
    }
  }

  // Dark theme (default)
  lines.push("  /* Dark theme colors (default) */");
  const darkKeys = Object.keys(tokens).filter((k) => k.startsWith("dark."));
  for (const k of darkKeys.sort()) {
    const v = tokens[k];
    if (v == null) continue;
    const name = k.replace("dark.", "").replace(/-/g, "-");
    lines.push(`  --sc-${name}: ${v};`);
  }

  // Component tokens — ALL components
  lines.push("  /* Component tokens */");
  for (const prefix of COMPONENT_PREFIXES) {
    const compKeys = Object.keys(tokens)
      .filter((k) => k.startsWith(`${prefix}.`))
      .sort();
    for (const k of compKeys) {
      const v = tokens[k];
      if (v != null) {
        const suffix = k.replace(`${prefix}.`, "").replace(/-/g, "-");
        lines.push(`  --sc-${prefix}-${suffix}: ${v};`);
      }
    }
  }

  lines.push("}");
  lines.push("");
  lines.push("@media (prefers-color-scheme: light) {");
  lines.push("  :root {");
  lines.push("    /* Light theme — state/interactive tokens */");
  const lightKeys = Object.keys(tokens).filter((k) => k.startsWith("light."));
  for (const k of lightKeys.sort()) {
    const v = tokens[k];
    if (v == null) continue;
    const name = k.replace("light.", "").replace(/-/g, "-");
    lines.push(`    --sc-${name}: ${v};`);
  }
  lines.push("  }");
  lines.push("}");
  lines.push("");

  // High-contrast theme
  const highContrastKeys = Object.keys(tokens).filter((k) =>
    k.startsWith("high-contrast."),
  );
  if (highContrastKeys.length > 0) {
    lines.push("@media (prefers-contrast: more) {");
    lines.push("  :root {");
    for (const k of highContrastKeys.sort()) {
      const v = tokens[k];
      if (v == null) continue;
      const name = k.replace("high-contrast.", "").replace(/-/g, "-");
      lines.push(`    --sc-${name}: ${v};`);
    }
    lines.push("  }");
    lines.push("}");
    lines.push("");
  }

  // Reduced motion
  lines.push("@media (prefers-reduced-motion: reduce) {");
  lines.push("  :root {");
  const durationKeys = Object.keys(tokens).filter((k) =>
    k.startsWith("duration."),
  );
  for (const k of durationKeys) {
    const suffix = k.replace("duration.", "");
    lines.push(`    --sc-duration-${suffix}: 0ms;`);
  }
  lines.push("  }");
  lines.push("}");

  return lines.join("\n");
}

function generateSwift(tokens: TokenMap): string {
  const lines: string[] = [
    "// Auto-generated from design-tokens/ — do not edit manually",
    "import SwiftUI",
    "",
    "public enum SCTokens {",
  ];

  // Dark colors (exclude shadows - they're CSS values, not colors)
  lines.push("    // MARK: - Colors (Dark)");
  lines.push("    public enum Dark {");
  const darkKeys = Object.keys(tokens).filter(
    (k) =>
      k.startsWith("dark.") &&
      !k.includes("shadow") &&
      typeof tokens[k] === "string" &&
      ((tokens[k] as string).startsWith("#") ||
        (tokens[k] as string).startsWith("rgba")),
  );
  for (const k of darkKeys.sort()) {
    const v = tokens[k] as string;
    if (!v.startsWith("#") && !v.startsWith("rgba")) continue;
    const name = toSwiftCase(k.replace("dark.", ""));
    const colorExpr = formatSwiftColor(v);
    lines.push(`        public static let ${name} = ${colorExpr}`);
  }
  lines.push("    }");
  lines.push("");

  // Light colors (exclude shadows)
  lines.push("    // MARK: - Colors (Light)");
  lines.push("    public enum Light {");
  const lightKeys = Object.keys(tokens).filter(
    (k) =>
      k.startsWith("light.") &&
      !k.includes("shadow") &&
      typeof tokens[k] === "string" &&
      ((tokens[k] as string).startsWith("#") ||
        (tokens[k] as string).startsWith("rgba")),
  );
  for (const k of lightKeys.sort()) {
    const v = tokens[k] as string;
    if (!v.startsWith("#") && !v.startsWith("rgba")) continue;
    const name = toSwiftCase(k.replace("light.", ""));
    const colorExpr = formatSwiftColor(v);
    lines.push(`        public static let ${name} = ${colorExpr}`);
  }
  lines.push("    }");
  lines.push("");

  // Spacing (px)
  lines.push("    // MARK: - Spacing");
  const spaceMap: Record<string, string> = {
    "spacing.xs": "spaceXs",
    "spacing.sm": "spaceSm",
    "spacing.md": "spaceMd",
    "spacing.lg": "spaceLg",
    "spacing.xl": "spaceXl",
    "spacing.2xl": "space2xl",
  };
  for (const [path, name] of Object.entries(spaceMap)) {
    const v = tokens[path] as string | undefined;
    if (v) {
      const px = dimToPx(v);
      if (px != null)
        lines.push(`    public static let ${name}: CGFloat = ${px}`);
    }
  }
  lines.push("");

  // Radius
  lines.push("    // MARK: - Radius");
  const radiusMap: Record<string, string> = {
    "radius.sm": "radiusSm",
    "radius.md": "radiusMd",
    "radius.lg": "radiusLg",
    "radius.xl": "radiusXl",
  };
  for (const [path, name] of Object.entries(radiusMap)) {
    const v = tokens[path] as string | undefined;
    if (v) {
      const px = dimToPx(v);
      if (px != null)
        lines.push(`    public static let ${name}: CGFloat = ${px}`);
    }
  }
  lines.push("");

  // Typography
  lines.push("    // MARK: - Typography");
  const fontSans = (tokens["fontFamily.sans"] as string) ?? "Avenir";
  lines.push(
    `    public static let fontSans = "${fontSans.split(",")[0].replace(/^'\s*|\s*'$/g, "")}"`,
  );
  const fontMono = (tokens["fontFamily.mono"] as string) ?? "Geist Mono";
  lines.push(
    `    public static let fontMono = "${fontMono.split(",")[0].replace(/^'\s*|\s*'$/g, "")}"`,
  );
  lines.push("");

  // Font sizes (px)
  lines.push("    // MARK: - Font sizes");
  const fontSizeKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("fontSize."))
    .sort();
  for (const k of fontSizeKeys) {
    const v = tokens[k] as string | undefined;
    if (!v) continue;
    const px = dimToPx(v);
    if (px != null) {
      const suffix = k.replace("fontSize.", "");
      const name =
        "text" +
        (suffix.match(/^\d/)
          ? suffix.charAt(0) +
            suffix.slice(1).replace(/^[a-z]/, (c) => c.toUpperCase())
          : suffix.charAt(0).toUpperCase() + suffix.slice(1).replace(/-/g, ""));
      lines.push(`    public static let ${name}: CGFloat = ${px}`);
    }
  }
  lines.push("");

  // Font weights
  lines.push("    // MARK: - Font weights");
  const fontWeightKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("fontWeight."))
    .sort();
  for (const k of fontWeightKeys) {
    const v = tokens[k] as number | undefined;
    if (v == null) continue;
    const name =
      "weight" +
      k
        .replace("fontWeight.", "")
        .replace(/-/g, "")
        .split(".")
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    public static let ${name}: CGFloat = ${v}`);
  }
  lines.push("");

  // Duration (seconds)
  lines.push("    // MARK: - Duration");
  const durationKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("duration."))
    .sort();
  for (const k of durationKeys) {
    const v = tokens[k] as string | undefined;
    if (!v) continue;
    const ms = parseDurationMs(v);
    if (ms != null) {
      const sec = (ms / 1000).toFixed(2).replace(/\.?0+$/, "");
      const name =
        "duration" +
        k
          .replace("duration.", "")
          .replace(/-/g, "")
          .split(".")
          .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
          .join("");
      lines.push(`    public static let ${name}: Double = ${sec}`);
    }
  }
  lines.push("");

  // Opacity
  lines.push("    // MARK: - Opacity");
  const opacityKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("opacity."))
    .sort();
  for (const k of opacityKeys) {
    const v = tokens[k] as number | undefined;
    if (v == null) continue;
    const name =
      "opacity" +
      k
        .replace("opacity.", "")
        .replace(/-/g, "")
        .split(".")
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    public static let ${name}: Double = ${v}`);
  }
  lines.push("");

  // Spring
  lines.push("    // MARK: - Motion (Spring)");
  const springNames = ["micro", "standard", "expressive", "dramatic"];
  for (const name of springNames) {
    const stiff = tokens[`spring.${name}.stiffness`] as number;
    const damp = tokens[`spring.${name}.damping`] as number;
    const mass = (tokens[`spring.${name}.mass`] as number) ?? 1;
    if (stiff == null || damp == null) continue;
    const response = springToSwiftResponse(stiff, damp, mass);
    const df = springToSwiftDampingFraction(stiff, damp, mass);
    lines.push(
      `    public static let spring${name.charAt(0).toUpperCase() + name.slice(1)} = Animation.spring(response: ${response}, dampingFraction: ${df})`,
    );
  }
  lines.push("}");
  lines.push("");
  lines.push("extension Color {");
  lines.push("    init(hex: UInt, alpha: Double = 1) {");
  lines.push("        self.init(");
  lines.push("            .sRGB,");
  lines.push("            red: Double((hex >> 16) & 0xFF) / 255,");
  lines.push("            green: Double((hex >> 8) & 0xFF) / 255,");
  lines.push("            blue: Double(hex & 0xFF) / 255,");
  lines.push("            opacity: alpha");
  lines.push("        )");
  lines.push("    }");
  lines.push("}");

  return lines.join("\n");
}

function generateKotlin(tokens: TokenMap): string {
  const lines: string[] = [
    "// Auto-generated from design-tokens/ — do not edit manually",
    "package ai.seaclaw.app.ui",
    "",
    "import androidx.compose.ui.graphics.Color",
    "import androidx.compose.ui.unit.dp",
    "import androidx.compose.ui.unit.sp",
    "",
    "object SCTokens {",
  ];

  // Dark
  lines.push("    // Colors (Dark)");
  lines.push("    object Dark {");
  const darkKeys = Object.keys(tokens).filter(
    (k) =>
      k.startsWith("dark.") &&
      !k.includes("shadow") &&
      typeof tokens[k] === "string" &&
      ((tokens[k] as string).startsWith("#") ||
        (tokens[k] as string).startsWith("rgba")),
  );
  for (const k of darkKeys.sort()) {
    const v = tokens[k] as string;
    const name = toKotlinCase(k.replace("dark.", ""));
    const color = v.startsWith("#") ? hexToKotlin(v) : colorToKotlin(v);
    lines.push(`        val ${name} = Color(${color})`);
  }
  lines.push("    }");
  lines.push("");

  // Light
  lines.push("    // Colors (Light)");
  lines.push("    object Light {");
  const lightKeys = Object.keys(tokens).filter(
    (k) =>
      k.startsWith("light.") &&
      !k.includes("shadow") &&
      typeof tokens[k] === "string" &&
      ((tokens[k] as string).startsWith("#") ||
        (tokens[k] as string).startsWith("rgba")),
  );
  for (const k of lightKeys.sort()) {
    const v = tokens[k] as string;
    const name = toKotlinCase(k.replace("light.", ""));
    const color = v.startsWith("#") ? hexToKotlin(v) : colorToKotlin(v);
    lines.push(`        val ${name} = Color(${color})`);
  }
  lines.push("    }");
  lines.push("");

  // Spacing
  lines.push("    // Spacing");
  const spaceMap: Record<string, string> = {
    "spacing.xs": "spaceXs",
    "spacing.sm": "spaceSm",
    "spacing.md": "spaceMd",
    "spacing.lg": "spaceLg",
    "spacing.xl": "spaceXl",
    "spacing.2xl": "space2xl",
  };
  for (const [path, name] of Object.entries(spaceMap)) {
    const v = tokens[path] as string | undefined;
    if (v) {
      const px = dimToPx(v);
      if (px != null) lines.push(`    val ${name} = ${px}.dp`);
    }
  }
  lines.push("");

  // Radius
  lines.push("    // Radius");
  const radiusMap: Record<string, string> = {
    "radius.sm": "radiusSm",
    "radius.md": "radiusMd",
    "radius.lg": "radiusLg",
    "radius.xl": "radiusXl",
  };
  for (const [path, name] of Object.entries(radiusMap)) {
    const v = tokens[path] as string | undefined;
    if (v) {
      const px = dimToPx(v);
      if (px != null) lines.push(`    val ${name} = ${px}.dp`);
    }
  }
  lines.push("");

  // Font sizes
  lines.push("    // Font sizes");
  const sizeMap: Record<string, string> = {
    "fontSize.xs": "textXs",
    "fontSize.sm": "textSm",
    "fontSize.base": "textBase",
    "fontSize.lg": "textLg",
    "fontSize.xl": "textXl",
  };
  for (const [path, name] of Object.entries(sizeMap)) {
    const v = tokens[path] as string | undefined;
    if (v) {
      const px = dimToPx(v);
      if (px != null) lines.push(`    val ${name} = ${px}.sp`);
    }
  }
  lines.push("");

  // Font weights
  lines.push("    // Font weights");
  const kotlinFontWeightKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("fontWeight."))
    .sort();
  for (const k of kotlinFontWeightKeys) {
    const v = tokens[k] as number | undefined;
    if (v == null) continue;
    const name =
      "weight" +
      k
        .replace("fontWeight.", "")
        .replace(/-/g, "")
        .split(".")
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    val ${name} = ${v}`);
  }
  lines.push("");

  // Line heights
  lines.push("    // Line heights");
  const lineHeightKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("lineHeight."))
    .sort();
  for (const k of lineHeightKeys) {
    const v = tokens[k] as number | undefined;
    if (v == null) continue;
    const name =
      "leading" +
      k
        .replace("lineHeight.", "")
        .replace(/-/g, "")
        .split(".")
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    val ${name} = ${v}f`);
  }
  lines.push("");

  // Letter spacing (em multiplier)
  lines.push("    // Letter spacing (em multiplier for LetterSpacing.Em())");
  const letterSpacingKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("letterSpacing."))
    .sort();
  for (const k of letterSpacingKeys) {
    const v = tokens[k] as string | undefined;
    if (!v) continue;
    const em = parseEmValue(v);
    if (em != null) {
      const name =
        "tracking" +
        k
          .replace("letterSpacing.", "")
          .replace(/-/g, "")
          .split(".")
          .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
          .join("");
      lines.push(`    val ${name} = ${em}f`);
    }
  }
  lines.push("");

  // Duration (milliseconds as Long)
  lines.push("    // Duration (milliseconds)");
  const kotlinDurationKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("duration."))
    .sort();
  for (const k of kotlinDurationKeys) {
    const v = tokens[k] as string | undefined;
    if (!v) continue;
    const ms = parseDurationMs(v);
    if (ms != null) {
      const name =
        "duration" +
        k
          .replace("duration.", "")
          .replace(/-/g, "")
          .split(".")
          .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
          .join("");
      lines.push(`    val ${name} = ${ms}L`);
    }
  }
  lines.push("");

  // Easing (as string descriptions for documentation)
  lines.push("    // Easing curves (CSS values for documentation)");
  const easingKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("easing."))
    .sort();
  for (const k of easingKeys) {
    const v = tokens[k] as string | undefined;
    if (!v) continue;
    const suffix = toKotlinSuffix(k.replace("easing.", ""));
    const name =
      "easing" +
      (suffix ? suffix.charAt(0).toUpperCase() + suffix.slice(1) : "");
    const escaped = (v as string).replace(/"/g, '\\"');
    lines.push(`    val ${name} = "${escaped}"`);
  }
  lines.push("");

  // Spring (stiffness, damping, mass)
  lines.push("    // Spring (stiffness, damping, mass)");
  const springNames = ["micro", "standard", "expressive", "dramatic"];
  for (const name of springNames) {
    const stiff = tokens[`spring.${name}.stiffness`] as number | undefined;
    const damp = tokens[`spring.${name}.damping`] as number | undefined;
    const mass = (tokens[`spring.${name}.mass`] as number | undefined) ?? 1;
    if (stiff == null || damp == null) continue;
    const capName = name.charAt(0).toUpperCase() + name.slice(1);
    lines.push(`    val spring${capName}Stiffness = ${stiff}f`);
    lines.push(`    val spring${capName}Damping = ${damp}f`);
    lines.push(`    val spring${capName}Mass = ${mass}f`);
  }
  lines.push("");

  // Opacity
  lines.push("    // Opacity");
  const kotlinOpacityKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("opacity."))
    .sort();
  for (const k of kotlinOpacityKeys) {
    const v = tokens[k] as number | undefined;
    if (v == null) continue;
    const suffix = toKotlinSuffix(k.replace("opacity.", ""));
    const name =
      "opacity" +
      (suffix ? suffix.charAt(0).toUpperCase() + suffix.slice(1) : "");
    lines.push(`    val ${name} = ${v}f`);
  }
  lines.push("}");
  return lines.join("\n");
}

function generateCHeader(tokens: TokenMap): string {
  const semanticToMacro: Array<[string, string]> = [
    ["dark.accent", "SC_COLOR_ACCENT"],
    ["dark.success", "SC_COLOR_SUCCESS"],
    ["dark.warning", "SC_COLOR_WARNING"],
    ["dark.error", "SC_COLOR_ERROR"],
    ["dark.info", "SC_COLOR_INFO"],
    ["dark.text-muted", "SC_COLOR_MUTED"],
    ["dark.text-faint", "SC_COLOR_FAINT"],
  ];
  const fallbacks: Record<string, number> = {
    SC_COLOR_ACCENT: 209,
    SC_COLOR_SUCCESS: 78,
    SC_COLOR_WARNING: 178,
    SC_COLOR_ERROR: 196,
    SC_COLOR_INFO: 69,
    SC_COLOR_MUTED: 245,
    SC_COLOR_FAINT: 240,
  };
  const colorLines: string[] = [];
  for (const [tokenKey, macro] of semanticToMacro) {
    const val = tokens[tokenKey];
    const code =
      typeof val === "string" && val.startsWith("#")
        ? hexToAnsi256(val)
        : fallbacks[macro];
    colorLines.push(`#define ${macro} "\\033[38;5;${code}m"`);
  }
  return `/* Auto-generated from design-tokens/ — do not edit manually */
#ifndef SC_DESIGN_TOKENS_H
#define SC_DESIGN_TOKENS_H

/* ANSI 256-color codes for semantic colors (from dark theme tokens) */
${colorLines.join("\n")}
#define SC_COLOR_RESET "\\033[0m"
#define SC_COLOR_BOLD "\\033[1m"
#define SC_COLOR_DIM "\\033[2m"

/* Box-drawing characters (UTF-8) */
#define SC_BOX_VERT "\\xe2\\x94\\x82"
#define SC_BOX_HORIZ "\\xe2\\x94\\x80"
#define SC_BOX_TL "\\xe2\\x94\\x8c"
#define SC_BOX_TR "\\xe2\\x94\\x90"
#define SC_BOX_BL "\\xe2\\x94\\x94"
#define SC_BOX_BR "\\xe2\\x94\\x98"
#define SC_CHEVRON "\\xe2\\x9d\\xaf"
#define SC_CHECK "\\xe2\\x9c\\x93"
#define SC_CROSS "\\xe2\\x9c\\x97"

#endif /* SC_DESIGN_TOKENS_H */
`;
}

function formatSwiftColor(val: string): string {
  if (val.startsWith("#")) {
    return `Color(hex: ${hexToSwift(val)})`;
  }
  const m = val.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/);
  if (m) {
    const r = Math.round((parseInt(m[1], 10) / 255) * 10000) / 10000;
    const g = Math.round((parseInt(m[2], 10) / 255) * 10000) / 10000;
    const b = Math.round((parseInt(m[3], 10) / 255) * 10000) / 10000;
    const a = m[4] ? Math.round(parseFloat(m[4]) * 10000) / 10000 : 1;
    return `Color(red: ${r}, green: ${g}, blue: ${b}, opacity: ${a})`;
  }
  return "Color(hex: 0x000000)";
}

function toSwiftCase(s: string): string {
  const parts = s.split("-");
  return (
    parts[0].toLowerCase() +
    parts
      .slice(1)
      .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
      .join("")
  );
}

function toKotlinCase(s: string): string {
  const parts = s.split("-");
  return (
    parts[0] +
    parts
      .slice(1)
      .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
      .join("")
  );
}

/** Convert token suffix to Kotlin camelCase (e.g. overlay-heavy -> overlayHeavy) */
function toKotlinSuffix(s: string): string {
  return s
    .split("-")
    .map((p, i) => (i === 0 ? p : p.charAt(0).toUpperCase() + p.slice(1)))
    .join("");
}

main();
