#!/usr/bin/env npx tsx
/**
 * Regenerate docs/tokens-studio.json from canonical *.tokens.json sources.
 * Run from repo root: npx tsx design-tokens/sync-tokens-studio.ts
 * `--out <path>` writes elsewhere (check-drift.sh diffs that against the committed file).
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, "..");
const DT = path.join(ROOT, "design-tokens");
const outFlag = process.argv.indexOf("--out");
const OUT =
  outFlag !== -1 && process.argv[outFlag + 1]
    ? path.resolve(process.argv[outFlag + 1])
    : path.join(ROOT, "docs/tokens-studio.json");

type TokenMap = Record<string, string | number>;

function collectTokens(obj: unknown, prefix = ""): TokenMap {
  const result: TokenMap = {};
  if (obj === null || typeof obj !== "object") return result;
  const rec = obj as Record<string, unknown>;
  for (const [key, val] of Object.entries(rec)) {
    if (key.startsWith("$")) continue;
    const pathPart = prefix ? `${prefix}.${key}` : key;
    if (val !== null && typeof val === "object" && "$value" in val) {
      const v = (val as { $value: string | number }).$value;
      result[pathPart] = v;
    } else if (typeof val === "object" && val !== null) {
      Object.assign(result, collectTokens(val, pathPart));
    }
  }
  return result;
}

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
        if (target !== undefined && typeof target === "string") {
          resolved[key] = target;
          changed = true;
        }
      }
    }
  }
  return resolved;
}

function stripSchema(obj: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    if (k === "$schema") continue;
    out[k] = v;
  }
  return out;
}

function loadJson(name: string): Record<string, unknown> {
  const p = path.join(DT, name);
  return JSON.parse(fs.readFileSync(p, "utf8")) as Record<string, unknown>;
}

function main(): void {
  const base = loadJson("base.tokens.json");
  const semantic = loadJson("semantic.tokens.json");
  const dataViz = loadJson("data-viz.tokens.json");
  const typography = loadJson("typography.tokens.json");
  const motion = loadJson("motion.tokens.json");
  const glass = loadJson("glass.tokens.json");
  const components = loadJson("components.tokens.json");
  const opacity = loadJson("opacity.tokens.json");
  const elevation = loadJson("elevation.tokens.json");
  const breakpoints = loadJson("breakpoints.tokens.json");

  const flat = collectTokens(base);
  const resolved = resolveRefs(flat);

  const chartCategorical = (dataViz.chart as Record<string, unknown>)
    .categorical as Record<string, unknown>;
  const resolvedCategorical: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(chartCategorical)) {
    if (k.startsWith("$")) {
      resolvedCategorical[k] = v;
      continue;
    }
    const token = v as { $value?: string; $type: string; $description?: string };
    if (typeof token.$value !== "string") continue;
    const ref = token.$value.match(/^\{([^}]+)\}$/);
    let outVal: string | number = token.$value;
    if (ref) {
      const target = resolved[ref[1]];
      if (typeof target === "string") outVal = target;
    }
    resolvedCategorical[k] = {
      $value: outVal,
      $type: token.$type,
      ...(token.$description != null ? { $description: token.$description } : {}),
    };
  }

  const studio: Record<string, unknown> = {
    base: {
      color: base.color,
      spacing: base.spacing,
      radius: base.radius,
      blur: base.blur,
      "z-index": base["z-index"],
    },
    "data-viz": {
      chart: {
        categorical: resolvedCategorical,
      },
    },
    semantic: stripSchema(semantic),
    typography: stripSchema(typography),
    motion: stripSchema(motion),
    glass: { glass: glass.glass },
    components: stripSchema(components),
    opacity: stripSchema(opacity),
    elevation: stripSchema(elevation),
    breakpoints: stripSchema(breakpoints),
  };

  fs.writeFileSync(OUT, JSON.stringify(studio, null, 2) + "\n");
  console.log("Wrote", OUT);
}

main();
