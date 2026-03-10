#!/usr/bin/env node
/**
 * Design tokens documentation generator
 * Reads W3C token JSON files and generates a human-readable markdown reference.
 * Run: npx tsx generate-docs.ts
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, "..");
const TOKENS_DIR = path.join(ROOT, "design-tokens");
const OUTPUT_PATH = path.join(ROOT, "docs", "design-tokens.md");

const TOKEN_FILES = [
  "base.tokens.json",
  "typography.tokens.json",
  "motion.tokens.json",
  "semantic.tokens.json",
  "components.tokens.json",
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

function main(): void {
  let tokens: TokenMap = {};
  for (const file of TOKEN_FILES) {
    const p = path.join(TOKENS_DIR, file);
    if (!fs.existsSync(p)) {
      console.error(`Missing token file: ${p}`);
      process.exit(1);
    }
    const data = JSON.parse(fs.readFileSync(p, "utf-8")) as Record<
      string,
      unknown
    >;
    tokens = { ...tokens, ...collectTokens(data) };
  }
  tokens = resolveRefs(tokens);

  const sections: string[] = [
    "# Design Tokens Reference",
    "",
    "Auto-generated from W3C token files in `design-tokens/`.",
    "",
    "---",
    "",
    "## Color Tokens",
    "",
    "| Token | Dark | Light |",
    "|-------|------|-------|",
  ];

  // Color tokens: dark.* and light.* (exclude shadow)
  const darkColorKeys = Object.keys(tokens)
    .filter(
      (k) =>
        k.startsWith("dark.") &&
        !k.includes("shadow") &&
        typeof tokens[k] === "string" &&
        ((tokens[k] as string).startsWith("#") ||
          (tokens[k] as string).startsWith("rgba")),
    )
    .sort();
  const lightColorKeys = Object.keys(tokens)
    .filter(
      (k) =>
        k.startsWith("light.") &&
        !k.includes("shadow") &&
        typeof tokens[k] === "string" &&
        ((tokens[k] as string).startsWith("#") ||
          (tokens[k] as string).startsWith("rgba")),
    )
    .sort();

  const colorNames = new Set([
    ...darkColorKeys.map((k) => k.replace("dark.", "")),
    ...lightColorKeys.map((k) => k.replace("light.", "")),
  ]);
  for (const name of [...colorNames].sort()) {
    const dark = tokens[`dark.${name}`] as string | undefined;
    const light = tokens[`light.${name}`] as string | undefined;
    sections.push(`| \`--hu-${name}\` | ${dark ?? "—"} | ${light ?? "—"} |`);
  }

  sections.push(
    "",
    "---",
    "",
    "## Spacing Tokens",
    "",
    "| Token | Value |",
    "|-------|-------|",
  );
  const spacingKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("spacing."))
    .sort();
  for (const k of spacingKeys) {
    const v = tokens[k];
    const name = k.replace("spacing.", "");
    sections.push(`| \`--hu-space-${name}\` | ${v} |`);
  }

  sections.push(
    "",
    "---",
    "",
    "## Radius Tokens",
    "",
    "| Token | Value |",
    "|-------|-------|",
  );
  const radiusKeys = Object.keys(tokens)
    .filter((k) => k.startsWith("radius."))
    .sort();
  for (const k of radiusKeys) {
    const v = tokens[k];
    const name = k.replace("radius.", "");
    sections.push(`| \`--hu-radius-${name}\` | ${v} |`);
  }

  sections.push(
    "",
    "---",
    "",
    "## Typography Tokens",
    "",
    "| Token | Value |",
    "|-------|-------|",
  );
  const typoKeys = [
    ...Object.keys(tokens).filter((k) => k.startsWith("fontFamily.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("fontSize.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("fontWeight.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("lineHeight.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("letterSpacing.")),
  ].sort();
  for (const k of typoKeys) {
    const v = tokens[k];
    let cssName = k;
    if (k.startsWith("fontFamily."))
      cssName = `--hu-font` + (k.endsWith(".mono") ? "-mono" : "");
    else if (k.startsWith("fontSize."))
      cssName = `--hu-text-${k.replace("fontSize.", "")}`;
    else if (k.startsWith("fontWeight."))
      cssName = `--hu-weight-${k.replace("fontWeight.", "")}`;
    else if (k.startsWith("lineHeight."))
      cssName = `--hu-leading-${k.replace("lineHeight.", "")}`;
    else if (k.startsWith("letterSpacing."))
      cssName = `--hu-tracking-${k.replace("letterSpacing.", "")}`;
    sections.push(`| \`${cssName}\` | ${v} |`);
  }

  sections.push(
    "",
    "---",
    "",
    "## Motion Tokens",
    "",
    "| Token | Value |",
    "|-------|-------|",
  );
  const motionKeys = [
    ...Object.keys(tokens).filter((k) => k.startsWith("duration.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("easing.")),
    ...Object.keys(tokens).filter((k) => k.startsWith("transition.")),
  ].sort();
  for (const k of motionKeys) {
    const v = tokens[k];
    let cssName = k;
    if (k.startsWith("duration."))
      cssName = `--hu-duration-${k.replace("duration.", "")}`;
    else if (k.startsWith("easing."))
      cssName = `--hu-${k.replace("easing.", "")}`;
    else if (k.startsWith("transition.")) cssName = "--hu-transition";
    sections.push(`| \`${cssName}\` | ${String(v).replace(/\|/g, "\\|")} |`);
  }
  const springKeys = Object.keys(tokens).filter((k) =>
    k.match(/^spring\.\w+\.(stiffness|damping|mass)$/),
  );
  const springNames = [
    ...new Set(springKeys.map((k) => k.split(".")[1])),
  ].sort();
  for (const name of springNames) {
    const stiff = tokens[`spring.${name}.stiffness`];
    const damp = tokens[`spring.${name}.damping`];
    const mass = tokens[`spring.${name}.mass`] ?? 1;
    sections.push(
      `| \`--hu-spring-${name}\` | stiffness: ${stiff}, damping: ${damp}, mass: ${mass} |`,
    );
  }

  sections.push(
    "",
    "---",
    "",
    "## Component Tokens",
    "",
    "| Token | Value |",
    "|-------|-------|",
  );
  const compKeys = Object.keys(tokens).filter(
    (k) =>
      k.startsWith("sidebar.") ||
      k.startsWith("button.") ||
      k.startsWith("card.") ||
      k.startsWith("modal.") ||
      k.startsWith("badge.") ||
      k.startsWith("toast."),
  );
  for (const k of compKeys.sort()) {
    const v = tokens[k];
    const cssName = `--hu-${k.replace(".", "-")}`;
    sections.push(`| \`${cssName}\` | ${v} |`);
  }

  const timestamp = new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
  sections.push("", "---", "", "_Generated: " + timestamp + "_");

  fs.mkdirSync(path.dirname(OUTPUT_PATH), { recursive: true });
  fs.writeFileSync(OUTPUT_PATH, sections.join("\n"), "utf-8");
  console.log("Wrote", OUTPUT_PATH);
}

main();
