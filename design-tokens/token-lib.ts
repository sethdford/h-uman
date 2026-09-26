import * as fs from "fs";
import * as path from "path";

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
  "spatial.tokens.json",
  "ambient.tokens.json",
  "3d.tokens.json",
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

export type Platform = "all" | "web";

export interface TokenSource {
  file: string;
  data: Record<string, unknown>;
}

/** Read and parse each token file; a missing file is a hard error. */
export function readTokenSources(
  dir: string,
  files: readonly string[],
): TokenSource[] {
  return files.map((file) => {
    const p = path.join(dir, file);
    if (!fs.existsSync(p)) throw new Error(`Missing token file: ${p}`);
    return {
      file,
      data: JSON.parse(fs.readFileSync(p, "utf-8")) as Record<string, unknown>,
    };
  });
}

/**
 * Which emitters a token file feeds. "all" = every platform (CSS, Swift,
 * Kotlin, C, docs JSON); "web" = CSS only. Every file must declare it —
 * a missing or unknown value is refused rather than defaulted, because a
 * silent default is how web-only values would leak into native outputs.
 */
export function platformOf(src: TokenSource): Platform {
  const ext = (src.data.$extensions ?? {}) as Record<string, unknown>;
  const p = ext["com.human.platform"];
  if (p === undefined) {
    throw new Error(
      `${src.file}: missing $extensions["com.human.platform"] (expected "all" or "web")`,
    );
  }
  if (p !== "all" && p !== "web") {
    throw new Error(
      `${src.file}: unknown com.human.platform ${JSON.stringify(p)} (expected "all" or "web")`,
    );
  }
  return p;
}

export function partitionByPlatform(sources: TokenSource[]): {
  shared: TokenSource[];
  web: TokenSource[];
} {
  return {
    shared: sources.filter((s) => platformOf(s) === "all"),
    web: sources.filter((s) => platformOf(s) === "web"),
  };
}

export { TOKEN_FILES, collectTokens, resolveRefs };
export type { TokenValue, TokenMap };
