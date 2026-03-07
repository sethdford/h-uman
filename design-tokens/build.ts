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
  "glass.tokens.json",
  "data-viz.tokens.json",
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

/**
 * Resolve {path.to.token} references in place; repeat until stable.
 * Gradient tokens (surface-gradient, surface-glow, etc.) use raw string values
 * and are emitted as-is in CSS — they contain linear-gradient/radial-gradient
 * and cannot be resolved like color tokens.
 */
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

function hexToRGB(hex: string): [number, number, number] | null {
  const m = hex.match(/^#([0-9a-fA-F]{6})$/);
  if (!m) return null;
  return [
    parseInt(m[1].substring(0, 2), 16),
    parseInt(m[1].substring(2, 4), 16),
    parseInt(m[1].substring(4, 6), 16),
  ];
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
  const hex = (((a << 24) | (r << 16) | (g << 8) | b) >>> 0)
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

function generateDart(tokens: TokenMap): string {
  const lines: string[] = [
    "// Auto-generated from design-tokens/ — do not edit manually",
    "import 'package:flutter/material.dart';",
    "",
    "abstract final class SCTokens {",
  ];

  for (const [key, val] of Object.entries(tokens).sort()) {
    const dartName = key
      .replace(/\./g, "_")
      .replace(/-/g, "_")
      .replace(/([A-Z])/g, "_$1")
      .toLowerCase()
      .replace(/^_/, "");

    if (typeof val === "string" && val.startsWith("#") && val.length === 7) {
      const hex = val.replace("#", "").toUpperCase();
      lines.push(`  static const ${dartName} = Color(0xFF${hex});`);
    } else if (typeof val === "number") {
      lines.push(`  static const ${dartName} = ${val};`);
    } else if (typeof val === "string" && val.endsWith("px")) {
      const num = parseFloat(val);
      if (!isNaN(num)) lines.push(`  static const ${dartName} = ${num};`);
    } else if (typeof val === "string" && val.endsWith("rem")) {
      const num = parseFloat(val) * 16;
      if (!isNaN(num)) lines.push(`  static const ${dartName} = ${num};`);
    } else if (typeof val === "string" && val.endsWith("ms")) {
      const num = parseInt(val);
      if (!isNaN(num))
        lines.push(
          `  static const ${dartName} = Duration(milliseconds: ${num});`,
        );
    }
  }

  lines.push("}");
  lines.push("");
  return lines.join("\n");
}

function generateRuntimeJSON(tokens: TokenMap): string {
  const organized: Record<string, Record<string, unknown>> = {};
  for (const [key, val] of Object.entries(tokens)) {
    const parts = key.split(".");
    const group = parts.length > 1 ? parts[0] : "base";
    const name = parts.length > 1 ? parts.slice(1).join(".") : key;
    if (!organized[group]) organized[group] = {};
    organized[group][name] = val;
  }
  return JSON.stringify(
    {
      $schema: "https://design-tokens.github.io/community-group/format/",
      tokens: organized,
    },
    null,
    2,
  );
}

function generateTypeScriptTokens(tokens: TokenMap): string {
  const lines: string[] = [
    "// Auto-generated from design-tokens/ — do not edit manually",
    "",
    "export const tokens = {",
  ];

  const groups: Record<string, Array<[string, unknown]>> = {};
  for (const [key, val] of Object.entries(tokens)) {
    const parts = key.split(".");
    const group = parts[0];
    const name = parts.slice(1).join(".");
    if (!groups[group]) groups[group] = [];
    groups[group].push([name || key, val]);
  }

  for (const [group, entries] of Object.entries(groups).sort()) {
    lines.push(`  ${toCamelCase(group)}: {`);
    for (const [name, val] of entries.sort()) {
      const camelName = toCamelCase(name);
      if (typeof val === "string") {
        lines.push(`    ${camelName}: ${JSON.stringify(val)},`);
      } else if (typeof val === "number") {
        lines.push(`    ${camelName}: ${val},`);
      }
    }
    lines.push("  },");
  }

  lines.push("} as const;");
  lines.push("");
  lines.push("export type TokenKey = keyof typeof tokens;");
  lines.push("");
  return lines.join("\n");
}

function toCamelCase(s: string): string {
  return s
    .split(/[-.]/)
    .map((p, i) => (i === 0 ? p : p.charAt(0).toUpperCase() + p.slice(1)))
    .join("");
}

function generateDocsReference(tokens: TokenMap): string {
  const groups: Record<
    string,
    Array<{ name: string; value: string; hex?: string }>
  > = {};
  for (const [key, val] of Object.entries(tokens)) {
    const parts = key.split(".");
    const group = parts.length > 1 ? parts.slice(0, -1).join(".") : "base";
    const name = parts[parts.length - 1];
    if (!groups[group]) groups[group] = [];
    const entry: { name: string; value: string; hex?: string } = {
      name,
      value: String(val),
    };
    if (typeof val === "string" && val.startsWith("#")) {
      entry.hex = val;
    }
    groups[group].push(entry);
  }
  return JSON.stringify(
    { generated: new Date().toISOString(), groups },
    null,
    2,
  );
}

function main() {
  let tokens: TokenMap = {};
  let p3Colors: Record<string, string> = {};
  for (const file of TOKEN_FILES) {
    const p = path.join(TOKENS_DIR, file);
    if (!fs.existsSync(p)) {
      console.error(`Missing token file: ${p}`);
      process.exit(1);
    }
    const data = JSON.parse(fs.readFileSync(p, "utf-8"));
    tokens = { ...tokens, ...collectTokens(data) };
    if (data.$extensions?.["seaclaw.p3Colors"]) {
      p3Colors = { ...p3Colors, ...data.$extensions["seaclaw.p3Colors"] };
    }
  }
  tokens = resolveRefs(tokens);

  const outdir = parseOutdir();

  const css = generateCSS(tokens, p3Colors);
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

  const refJson = generateDocsReference(tokens);
  const refPath = outdir
    ? path.join(outdir, "design-tokens-reference.json")
    : path.join(ROOT, "docs", "design-tokens-reference.json");
  const refDir = path.dirname(refPath);
  if (!fs.existsSync(refDir)) fs.mkdirSync(refDir, { recursive: true });
  writeOutput(outdir, refPath, "design-tokens-reference.json", refJson);

  const dart = generateDart(tokens);
  const dartDir = outdir || path.join(ROOT, "apps", "flutter", "lib");
  if (!fs.existsSync(dartDir)) fs.mkdirSync(dartDir, { recursive: true });
  writeOutput(
    outdir,
    path.join(dartDir, "design_tokens.dart"),
    "design_tokens.dart",
    dart,
  );

  const runtimeJson = generateRuntimeJSON(tokens);
  writeOutput(
    outdir,
    outdir
      ? path.join(outdir, "tokens.json")
      : path.join(ROOT, "docs", "tokens.json"),
    "tokens.json",
    runtimeJson,
  );

  const tsTokens = generateTypeScriptTokens(tokens);
  writeOutput(
    outdir,
    outdir
      ? path.join(outdir, "tokens.ts")
      : path.join(ROOT, "docs", "tokens.ts"),
    "tokens.ts",
    tsTokens,
  );

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

function generateCSS(
  tokens: TokenMap,
  p3Colors: Record<string, string> = {},
): string {
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

  // Blur
  lines.push("  /* Blur */");
  const blurKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("blur."))
    .sort();
  for (const k of blurKeys) {
    const v = tokens[k];
    if (v != null) lines.push(`  --sc-blur-${k.replace("blur.", "")}: ${v};`);
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

  // Glass tiers
  lines.push("  /* Glass tiers (Liquid Motion) */");
  const glassTiers = ["subtle", "standard", "prominent"];
  for (const tier of glassTiers) {
    const glassProps = [
      "blur",
      "saturate",
      "bg-opacity",
      "border-opacity",
      "inset-opacity",
      "tint-opacity",
      "refraction-scale",
    ];
    for (const prop of glassProps) {
      const val = tokens[`glass.${tier}.${prop}`];
      if (val != null) lines.push(`  --sc-glass-${tier}-${prop}: ${val};`);
    }
  }

  // Glass: dynamic-light, vibrancy, interactive
  lines.push("  /* Glass: dynamic-light, vibrancy, interactive */");
  const glassExtraGroups = ["dynamic-light", "vibrancy", "interactive"];
  for (const group of glassExtraGroups) {
    const groupKeys = Object.keys(tokens)
      .filter((k) => k.startsWith(`${group}.`))
      .sort();
    for (const k of groupKeys) {
      const val = tokens[k];
      if (val != null) {
        const prop = k.replace(`${group}.`, "");
        lines.push(`  --sc-glass-${group}-${prop}: ${val};`);
      }
    }
  }

  // Micro-physics presets
  lines.push("  /* Pixar micro-physics */");
  const microPhysicsKeys = Object.keys(tokens).filter((k) =>
    k.startsWith("micro-physics."),
  );
  for (const k of microPhysicsKeys.sort()) {
    const v = tokens[k];
    if (v != null) {
      const name = k.replace("micro-physics.", "").replace(/\./g, "-");
      lines.push(`  --sc-physics-${name}: ${v};`);
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

  // Container transforms, path motion, view transitions
  const advancedMotionGroups = [
    "container-transform",
    "path-motion",
    "view-transition",
  ];
  for (const group of advancedMotionGroups) {
    const groupKeys = Object.keys(tokens).filter((k) =>
      k.startsWith(`${group}.`),
    );
    if (groupKeys.length > 0) {
      lines.push(`  /* ${group.replace(/-/g, " ")} */`);
      for (const k of groupKeys.sort()) {
        const val = tokens[k];
        if (val != null) {
          const prop = k.replace(`${group}.`, "");
          lines.push(`  --sc-${group}-${prop}: ${val};`);
        }
      }
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

  // data-theme selectors for catalog theme toggle (same tokens as :root / @media)
  lines.push('[data-theme="dark"] {');
  lines.push("  /* Dark theme — explicit override for theme switcher */");
  for (const k of darkKeys.sort()) {
    const v = tokens[k];
    if (v == null) continue;
    const name = k.replace("dark.", "").replace(/-/g, "-");
    lines.push(`  --sc-${name}: ${v};`);
  }
  lines.push("}");
  lines.push("");
  lines.push('[data-theme="light"] {');
  lines.push("  /* Light theme — explicit override for theme switcher */");
  for (const k of lightKeys.sort()) {
    const v = tokens[k];
    if (v == null) continue;
    const name = k.replace("light.", "").replace(/-/g, "-");
    lines.push(`  --sc-${name}: ${v};`);
  }
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

  // Wide gamut (P3) color overrides
  const p3Entries = Object.entries(p3Colors);
  if (p3Entries.length > 0) {
    lines.push("");
    lines.push("@media (color-gamut: p3) {");
    lines.push("  :root {");
    lines.push(
      "    /* Wide gamut P3 color overrides — more vivid on supported displays */",
    );
    const darkP3 = p3Entries.filter(([k]) => k.startsWith("dark."));
    for (const [k, v] of darkP3.sort()) {
      const name = k.replace("dark.", "").replace(/-/g, "-");
      lines.push(`    --sc-${name}: ${v};`);
    }
    lines.push("  }");
    const lightP3 = p3Entries.filter(([k]) => k.startsWith("light."));
    if (lightP3.length > 0) {
      lines.push("  @media (prefers-color-scheme: light) {");
      lines.push("    :root {");
      for (const [k, v] of lightP3.sort()) {
        const name = k.replace("light.", "").replace(/-/g, "-");
        lines.push(`      --sc-${name}: ${v};`);
      }
      lines.push("    }");
      lines.push("  }");
    }
    lines.push("}");
  }

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
        .split(/[.\-]/)
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    public static let ${name}: Double = ${v}`);
  }
  lines.push("");

  // Glass tokens (glass.*, dynamic-light.*, vibrancy.*, interactive.*)
  lines.push("    // MARK: - Glass");
  const glassPrefixes = [
    "glass.",
    "dynamic-light.",
    "vibrancy.",
    "interactive.",
  ];
  const glassNumericKeys = Object.keys(tokens)
    .filter(
      (k) =>
        glassPrefixes.some((p) => k.startsWith(p)) &&
        typeof tokens[k] === "number",
    )
    .sort();
  for (const k of glassNumericKeys) {
    const v = tokens[k] as number;
    const suffix = k.replace(
      /^(glass\.|dynamic-light\.|vibrancy\.|interactive\.)/,
      "",
    );
    const name =
      "glass" +
      suffix
        .split(/[.\-]/)
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    public static let ${name}: Double = ${v}`);
  }
  // Glass dimension tokens (blur, glow spread, etc.)
  const glassDimKeys = Object.keys(tokens)
    .filter(
      (k) =>
        glassPrefixes.some((p) => k.startsWith(p)) &&
        typeof tokens[k] === "string" &&
        (tokens[k] as string).endsWith("px"),
    )
    .sort();
  for (const k of glassDimKeys) {
    const v = tokens[k] as string;
    const px = parseInt(v);
    if (!isNaN(px)) {
      const suffix = k.replace(
        /^(glass\.|dynamic-light\.|vibrancy\.|interactive\.)/,
        "",
      );
      const name =
        "glass" +
        suffix
          .split(/[.\-]/)
          .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
          .join("");
      lines.push(`    public static let ${name}: CGFloat = ${px}`);
    }
  }
  lines.push("");

  // Spring (discover from tokens)
  lines.push("    // MARK: - Motion (Spring)");
  const springNames = Object.keys(tokens)
    .filter((k) => k.match(/^spring\.[\w-]+\.stiffness$/))
    .map((k) => k.split(".")[1]);
  for (const name of springNames) {
    const stiff = tokens[`spring.${name}.stiffness`] as number | undefined;
    const damp = tokens[`spring.${name}.damping`] as number;
    const mass = (tokens[`spring.${name}.mass`] as number) ?? 1;
    if (stiff == null || damp == null) continue;
    const response = springToSwiftResponse(stiff, damp, mass);
    const df = springToSwiftDampingFraction(stiff, damp, mass);
    lines.push(
      `    public static let spring${name.charAt(0).toUpperCase() + name.slice(1)} = Animation.spring(response: ${response}, dampingFraction: ${df})`,
    );
  }
  lines.push("");

  lines.push("    // MARK: - Haptic Feedback");
  lines.push("    public enum Haptic {");
  lines.push("        case light, medium, heavy, success, warning, selection");
  lines.push("");
  lines.push("        public func trigger() {");
  lines.push("            #if canImport(UIKit)");
  lines.push("            switch self {");
  lines.push("            case .light:");
  lines.push(
    "                UIImpactFeedbackGenerator(style: .light).impactOccurred()",
  );
  lines.push("            case .medium:");
  lines.push(
    "                UIImpactFeedbackGenerator(style: .medium).impactOccurred()",
  );
  lines.push("            case .heavy:");
  lines.push(
    "                UIImpactFeedbackGenerator(style: .heavy).impactOccurred()",
  );
  lines.push("            case .success:");
  lines.push(
    "                UINotificationFeedbackGenerator().notificationOccurred(.success)",
  );
  lines.push("            case .warning:");
  lines.push(
    "                UINotificationFeedbackGenerator().notificationOccurred(.warning)",
  );
  lines.push("            case .selection:");
  lines.push(
    "                UISelectionFeedbackGenerator().selectionChanged()",
  );
  lines.push("            }");
  lines.push("            #endif");
  lines.push("        }");
  lines.push("    }");

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

  // Spring (stiffness, damping, mass) — discover from tokens
  lines.push("    // Spring (stiffness, damping, mass)");
  const kotlinSpringNames = Object.keys(tokens)
    .filter((k) => k.match(/^spring\.[\w-]+\.stiffness$/))
    .map((k) => k.split(".")[1]);
  for (const name of kotlinSpringNames) {
    const stiff = tokens[`spring.${name}.stiffness`] as number | undefined;
    const damp = tokens[`spring.${name}.damping`] as number | undefined;
    const mass = (tokens[`spring.${name}.mass`] as number | undefined) ?? 1;
    if (stiff == null || damp == null) continue;
    const capName = toPascalCase(name);
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
  lines.push("");

  // Glass tokens (glass.*, dynamic-light.*, vibrancy.*, interactive.*)
  lines.push("    // Glass");
  const kotlinGlassPrefixes = [
    "glass.",
    "dynamic-light.",
    "vibrancy.",
    "interactive.",
  ];
  const kotlinGlassNumericKeys = Object.keys(tokens)
    .filter(
      (k) =>
        kotlinGlassPrefixes.some((p) => k.startsWith(p)) &&
        typeof tokens[k] === "number",
    )
    .sort();
  for (const k of kotlinGlassNumericKeys) {
    const v = tokens[k] as number;
    const suffix = k.replace(
      /^(glass\.|dynamic-light\.|vibrancy\.|interactive\.)/,
      "",
    );
    const name =
      "glass" +
      suffix
        .split(/[.\-]/)
        .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
        .join("");
    lines.push(`    val ${name} = ${v}f`);
  }
  const kotlinGlassDimKeys = Object.keys(tokens)
    .filter(
      (k) =>
        kotlinGlassPrefixes.some((p) => k.startsWith(p)) &&
        typeof tokens[k] === "string" &&
        (tokens[k] as string).endsWith("px"),
    )
    .sort();
  for (const k of kotlinGlassDimKeys) {
    const v = tokens[k] as string;
    const px = parseInt(v);
    if (!isNaN(px)) {
      const suffix = k.replace(
        /^(glass\.|dynamic-light\.|vibrancy\.|interactive\.)/,
        "",
      );
      const name =
        "glass" +
        suffix
          .split(/[.\-]/)
          .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
          .join("");
      lines.push(`    val ${name} = ${px}.dp`);
    }
  }
  lines.push("");

  lines.push("    // Haptic Feedback");
  lines.push("    enum class HapticType {");
  lines.push("        LIGHT, MEDIUM, HEAVY, SUCCESS, WARNING, SELECTION;");
  lines.push("");
  lines.push("        fun toAndroidConstant(): Int = when (this) {");
  lines.push(
    "            LIGHT -> android.view.HapticFeedbackConstants.KEYBOARD_TAP",
  );
  lines.push(
    "            MEDIUM -> android.view.HapticFeedbackConstants.CONTEXT_CLICK",
  );
  lines.push(
    "            HEAVY -> android.view.HapticFeedbackConstants.LONG_PRESS",
  );
  lines.push(
    "            SUCCESS -> android.view.HapticFeedbackConstants.CONFIRM",
  );
  lines.push(
    "            WARNING -> android.view.HapticFeedbackConstants.REJECT",
  );
  lines.push(
    "            SELECTION -> android.view.HapticFeedbackConstants.CLOCK_TICK",
  );
  lines.push("        }");
  lines.push("    }");

  lines.push("}");
  return lines.join("\n");
}

function generateCHeader(tokens: TokenMap): string {
  /* All semantic color tokens to emit, grouped by theme */
  const darkTokens: Array<[string, string]> = [
    ["dark.bg", "BG"],
    ["dark.bg-inset", "BG_INSET"],
    ["dark.bg-surface", "BG_SURFACE"],
    ["dark.bg-elevated", "BG_ELEVATED"],
    ["dark.bg-overlay", "BG_OVERLAY"],
    ["dark.text", "TEXT"],
    ["dark.text-muted", "TEXT_MUTED"],
    ["dark.text-faint", "TEXT_FAINT"],
    ["dark.accent", "ACCENT"],
    ["dark.accent-text", "ACCENT_TEXT"],
    ["dark.accent-hover", "ACCENT_HOVER"],
    ["dark.accent-strong", "ACCENT_STRONG"],
    ["dark.on-accent", "ON_ACCENT"],
    ["dark.accent-secondary", "ACCENT_SECONDARY"],
    ["dark.accent-tertiary", "ACCENT_TERTIARY"],
    ["dark.border", "BORDER"],
    ["dark.border-subtle", "BORDER_SUBTLE"],
    ["dark.success", "SUCCESS"],
    ["dark.warning", "WARNING"],
    ["dark.error", "ERROR"],
    ["dark.info", "INFO"],
    ["dark.focus-ring", "FOCUS_RING"],
    ["dark.link", "LINK"],
  ];
  const lightTokens: Array<[string, string]> = [
    ["light.bg", "BG"],
    ["light.bg-inset", "BG_INSET"],
    ["light.bg-surface", "BG_SURFACE"],
    ["light.bg-elevated", "BG_ELEVATED"],
    ["light.bg-overlay", "BG_OVERLAY"],
    ["light.text", "TEXT"],
    ["light.text-muted", "TEXT_MUTED"],
    ["light.text-faint", "TEXT_FAINT"],
    ["light.accent", "ACCENT"],
    ["light.accent-text", "ACCENT_TEXT"],
    ["light.accent-hover", "ACCENT_HOVER"],
    ["light.accent-strong", "ACCENT_STRONG"],
    ["light.on-accent", "ON_ACCENT"],
    ["light.accent-secondary", "ACCENT_SECONDARY"],
    ["light.accent-tertiary", "ACCENT_TERTIARY"],
    ["light.border", "BORDER"],
    ["light.border-subtle", "BORDER_SUBTLE"],
    ["light.success", "SUCCESS"],
    ["light.warning", "WARNING"],
    ["light.error", "ERROR"],
    ["light.info", "INFO"],
    ["light.focus-ring", "FOCUS_RING"],
    ["light.link", "LINK"],
  ];

  function emitColorMacros(
    entries: Array<[string, string]>,
    prefix: string,
    section: string,
  ): string {
    const lines: string[] = [];
    lines.push(`/* ${section} — ANSI 256-color foreground */`);
    for (const [key, name] of entries) {
      const macro = `${prefix}${name}`;
      const val = tokens[key];
      const rgb = typeof val === "string" ? hexToRGB(val) : null;
      const code = rgb ? hexToAnsi256(val as string) : 7;
      lines.push(`#define ${macro.padEnd(40)} "\\033[38;5;${code}m"`);
    }
    lines.push("");
    lines.push(`/* ${section} — truecolor (24-bit) foreground */`);
    for (const [key, name] of entries) {
      const macro = `${prefix}${name}_TC`;
      const val = tokens[key];
      const rgb = typeof val === "string" ? hexToRGB(val) : null;
      if (rgb) {
        lines.push(
          `#define ${macro.padEnd(40)} "\\033[38;2;${rgb[0]};${rgb[1]};${rgb[2]}m"`,
        );
      } else {
        lines.push(`#define ${macro.padEnd(40)} "\\033[38;5;7m"`);
      }
    }
    lines.push("");
    lines.push(`/* ${section} — truecolor (24-bit) background */`);
    for (const [key, name] of entries) {
      const macro = `${prefix}BG_${name}_TC`;
      const val = tokens[key];
      const rgb = typeof val === "string" ? hexToRGB(val) : null;
      if (rgb) {
        lines.push(
          `#define ${macro.padEnd(40)} "\\033[48;2;${rgb[0]};${rgb[1]};${rgb[2]}m"`,
        );
      } else {
        lines.push(`#define ${macro.padEnd(40)} "\\033[48;5;7m"`);
      }
    }
    lines.push("");
    lines.push(`/* ${section} — ANSI 256-color background */`);
    for (const [key, name] of entries) {
      const macro = `${prefix}BG_${name}`;
      const val = tokens[key];
      const rgb = typeof val === "string" ? hexToRGB(val) : null;
      const code = rgb ? hexToAnsi256(val as string) : 7;
      lines.push(`#define ${macro.padEnd(40)} "\\033[48;5;${code}m"`);
    }
    return lines.join("\n");
  }

  const darkSection = emitColorMacros(darkTokens, "SC_COLOR_", "Dark theme");
  const lightSection = emitColorMacros(
    lightTokens,
    "SC_COLOR_LIGHT_",
    "Light theme",
  );

  /* Legacy aliases for backward compatibility */
  const legacyAliases = [
    ["SC_COLOR_MUTED", "SC_COLOR_TEXT_MUTED"],
    ["SC_COLOR_FAINT", "SC_COLOR_TEXT_FAINT"],
  ];
  const aliasLines = legacyAliases
    .map(([old, cur]) => `#define ${old.padEnd(40)} ${cur}`)
    .join("\n");

  return `/* Auto-generated from design-tokens/ — do not edit manually */
#ifndef SC_DESIGN_TOKENS_H
#define SC_DESIGN_TOKENS_H

/*
 * Color macros come in four variants per token:
 *   SC_COLOR_<NAME>        — 256-color foreground (widest terminal compat)
 *   SC_COLOR_<NAME>_TC     — truecolor (24-bit) foreground
 *   SC_COLOR_BG_<NAME>     — 256-color background
 *   SC_COLOR_BG_<NAME>_TC  — truecolor (24-bit) background
 *
 * Use sc_terminal_color_level() from <seaclaw/terminal.h> to pick the right
 * variant at runtime, or use sc_color_fg()/sc_color_bg() for dynamic colors.
 */

${darkSection}

${lightSection}

/* Legacy aliases */
${aliasLines}

/* Formatting */
#define SC_COLOR_RESET               "\\033[0m"
#define SC_COLOR_BOLD                "\\033[1m"
#define SC_COLOR_DIM                 "\\033[2m"
#define SC_COLOR_ITALIC              "\\033[3m"
#define SC_COLOR_UNDERLINE           "\\033[4m"
#define SC_COLOR_STRIKETHROUGH       "\\033[9m"

/* Box-drawing characters (UTF-8) */
#define SC_BOX_VERT  "\\xe2\\x94\\x82"
#define SC_BOX_HORIZ "\\xe2\\x94\\x80"
#define SC_BOX_TL    "\\xe2\\x94\\x8c"
#define SC_BOX_TR    "\\xe2\\x94\\x90"
#define SC_BOX_BL    "\\xe2\\x94\\x94"
#define SC_BOX_BR    "\\xe2\\x94\\x98"
#define SC_BOX_TEE_R "\\xe2\\x94\\x9c"
#define SC_BOX_TEE_L "\\xe2\\x94\\xa4"
#define SC_BOX_CROSS "\\xe2\\x94\\xbc"
#define SC_CHEVRON   "\\xe2\\x9d\\xaf"
#define SC_CHECK     "\\xe2\\x9c\\x93"
#define SC_CROSS     "\\xe2\\x9c\\x97"
#define SC_BULLET    "\\xe2\\x80\\xa2"
#define SC_ELLIPSIS  "\\xe2\\x80\\xa6"
#define SC_ARROW_R   "\\xe2\\x86\\x92"

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

/** Convert name to PascalCase (e.g. standard -> Standard, some-name -> SomeName) */
function toPascalCase(s: string): string {
  return s
    .split("-")
    .map((p) => p.charAt(0).toUpperCase() + p.slice(1).toLowerCase())
    .join("");
}

/** Convert token suffix to Kotlin camelCase (e.g. overlay-heavy -> overlayHeavy) */
function toKotlinSuffix(s: string): string {
  return s
    .split("-")
    .map((p, i) => (i === 0 ? p : p.charAt(0).toUpperCase() + p.slice(1)))
    .join("");
}

main();
