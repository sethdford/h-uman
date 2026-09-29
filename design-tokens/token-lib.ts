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
  "quiet.tokens.json",
];

type TokenValue = string | number;
type TokenMap = Record<string, TokenValue>;
type TypeMap = Record<string, string>;

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
 * Path -> $type for every token, parallel to collectTokens. A token without
 * its own $type inherits the nearest enclosing group's $type (W3C DTCG).
 * Emitters select by this map, never by how a value happens to be spelled:
 * a color written `transparent` or `var(--x)` is still a color, and must be
 * refused by an emitter that cannot convert it rather than skipped.
 */
function collectTypes(obj: unknown, prefix = "", inherited?: string): TypeMap {
  const result: TypeMap = {};
  if (obj === null || typeof obj !== "object") return result;
  const rec = obj as Record<string, unknown>;

  for (const [key, val] of Object.entries(rec)) {
    if (key.startsWith("$")) continue;
    if (val === null || typeof val !== "object") continue;
    const node = val as Record<string, unknown>;
    const pathPart = prefix ? `${prefix}.${key}` : key;
    const own = typeof node.$type === "string" ? node.$type : undefined;
    if ("$value" in node) {
      const t = own ?? inherited;
      if (t !== undefined) result[pathPart] = t;
    } else {
      Object.assign(result, collectTypes(node, pathPart, own ?? inherited));
    }
  }
  return result;
}

const WHOLE_REF = /^\{([^}]+)\}$/;

/**
 * Resolve whole-value {path.to.token} references; repeat until stable.
 * A reference to a path that does not exist, or a reference cycle, throws
 * naming the token: leaving the literal `{ref}` in place let it reach the
 * emitters as a value no selector recognised. Braces inside a larger value
 * (gradients) are not references and are left alone.
 */
function resolveRefs(tokens: TokenMap): TokenMap {
  const resolved = { ...tokens };
  let changed = true;
  while (changed) {
    changed = false;
    for (const [key, val] of Object.entries(resolved)) {
      if (typeof val !== "string") continue;
      const ref = val.match(WHOLE_REF);
      if (!ref) continue;
      if (!Object.hasOwn(resolved, ref[1])) {
        throw new Error(
          `${key}: unresolved reference {${ref[1]}} (no token at that path)`,
        );
      }
      const target = resolved[ref[1]];
      if (target !== val) {
        resolved[key] = target;
        changed = true;
      }
    }
  }
  const cyclic = Object.keys(resolved).filter(
    (k) => typeof resolved[k] === "string" && WHOLE_REF.test(resolved[k] as string),
  );
  if (cyclic.length > 0) {
    throw new Error(`circular token references: ${cyclic.join(", ")}`);
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

/**
 * `--outdir DIR` / `--outdir=DIR` from a generator's argv, or null. With it, a
 * generator writes each output flat into DIR instead of its committed path, so
 * check-drift.sh can regenerate into a temp dir and diff.
 *
 * A malformed flag throws rather than returning null: null means "write the
 * committed files", so `--outdir --help` must not quietly overwrite docs/.
 */
export function parseOutdir(argv: string[] = process.argv.slice(2)): string | null {
  for (let i = 0; i < argv.length; i++) {
    let dir: string | undefined;
    if (argv[i] === "--outdir") {
      dir = argv[i + 1];
      if (dir?.startsWith("-")) dir = undefined; // next token is a flag
    } else if (argv[i].startsWith("--outdir=")) {
      dir = argv[i].slice("--outdir=".length);
    } else {
      continue;
    }
    if (!dir) {
      throw new Error(
        "--outdir needs a directory (use --outdir=DIR for a path starting with '-')",
      );
    }
    return dir;
  }
  return null;
}

export { TOKEN_FILES, collectTokens, collectTypes, resolveRefs };
export type { TokenValue, TokenMap, TypeMap };
