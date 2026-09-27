#!/usr/bin/env npx tsx
/**
 * Regenerate docs/tokens-studio.json from canonical *.tokens.json sources.
 * Run from repo root: npx tsx design-tokens/sync-tokens-studio.ts [--outdir DIR]
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";
import { collectTokens, parseOutdir, resolveRefs } from "./token-lib.js";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(SCRIPT_DIR, "..");
const DT = path.join(ROOT, "design-tokens");
const OUTDIR = parseOutdir();
const OUT = OUTDIR
  ? path.join(OUTDIR, "tokens-studio.json")
  : path.join(ROOT, "docs/tokens-studio.json");

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
      // A dangling ref used to be written out verbatim as `{path}`.
      if (!Object.hasOwn(resolved, ref[1])) {
        throw new Error(
          `data-viz chart.categorical.${k}: unresolved reference {${ref[1]}} (no token at that path in base.tokens.json)`,
        );
      }
      outVal = resolved[ref[1]];
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
