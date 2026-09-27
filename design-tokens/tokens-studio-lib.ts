/**
 * Tokens Studio export: the one implementation behind docs/tokens-studio.json.
 * sync-tokens-studio.ts and `figma-sync.ts --export` both call
 * writeTokensStudio(), so the two entry points cannot drift apart.
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";
import { collectTokens, parseOutdir, resolveRefs } from "./token-lib.js";

const TOKENS_DIR = path.dirname(fileURLToPath(import.meta.url));

/**
 * `--out FILE` (an exact path, resolved against cwd), else tokens-studio.json
 * inside `--outdir DIR`, else docs/tokens-studio.json. A malformed `--out`
 * throws for the same reason parseOutdir does: falling back would overwrite
 * the committed file.
 */
export function tokensStudioOutPath(
  argv: string[] = process.argv.slice(2),
): string {
  const i = argv.indexOf("--out");
  if (i !== -1) {
    const file = argv[i + 1];
    if (!file || file.startsWith("-")) {
      throw new Error("--out needs a file path");
    }
    return path.resolve(file);
  }
  const outdir = parseOutdir(argv);
  return outdir
    ? path.join(outdir, "tokens-studio.json")
    : path.join(TOKENS_DIR, "..", "docs", "tokens-studio.json");
}

function stripSchema(obj: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    if (k === "$schema") continue;
    out[k] = v;
  }
  return out;
}

export function buildTokensStudio(
  tokensDir: string = TOKENS_DIR,
): Record<string, unknown> {
  const loadJson = (name: string) =>
    JSON.parse(fs.readFileSync(path.join(tokensDir, name), "utf8")) as Record<
      string,
      unknown
    >;
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

  return {
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
}

export function writeTokensStudio(outPath: string = tokensStudioOutPath()): string {
  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify(buildTokensStudio(), null, 2) + "\n");
  return outPath;
}
