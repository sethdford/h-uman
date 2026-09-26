/**
 * Emits the Quiet Room layer (spec II.2). Opt in with data-brand="quiet" on
 * <html> or any container. :is() takes its most specific argument, so every
 * selector here scores ≥ (0,2,0) — above the later `@media (color-gamut: p3)
 * { :root {…} }` block at (0,1,0) that applies on essentially every Mac.
 */
import type { TokenMap } from "./token-lib.js";

export const QUIET_SCOPE =
  ':is(:root[data-brand="quiet"], [data-brand="quiet"])';
export const QUIET_DARK_AUTO =
  ':is(:root:not([data-theme="light"])[data-brand="quiet"], ' +
  ':root:not([data-theme="light"]) [data-brand="quiet"]):not([data-theme="light"])';
// Explicit themes. :is() takes its MOST specific argument whichever one
// matched, so "self" and "ancestor" forms must be separate rules: all four
// score (0,2,0), and the self forms are emitted LAST so an element's own
// data-theme beats an ancestor's (e.g. a dark specimen panel on a page the
// visitor toggled to light).
export const QUIET_DARK_ANCESTOR = '[data-theme="dark"] [data-brand="quiet"]';
// Under supported configurations (data-theme on <html> or on the quiet
// element itself), QUIET_LIGHT_ANCESTOR never actually decides the outcome:
// QUIET_SCOPE already yields light unconditionally, and QUIET_DARK_AUTO
// excludes a light-marked root via its own :not([data-theme="light"]), so
// there's no default it needs to override. It's emitted for light/dark
// symmetry with QUIET_DARK_ANCESTOR, not because a supported configuration
// depends on it.
export const QUIET_LIGHT_ANCESTOR = '[data-theme="light"] [data-brand="quiet"]';
export const QUIET_DARK_SELF = '[data-theme="dark"][data-brand="quiet"]';
export const QUIET_LIGHT_SELF = '[data-theme="light"][data-brand="quiet"]';

const PATH = /^quiet\.(light|dark|type)\.[a-z0-9-]+$/;

function group(web: TokenMap, prefix: string): Map<string, string> {
  const m = new Map<string, string>();
  for (const k of Object.keys(web).sort()) {
    if (k.startsWith(prefix)) m.set(k.slice(prefix.length), String(web[k]));
  }
  return m;
}

const decls = (m: Map<string, string>, indent: string) =>
  [...m].map(([n, v]) => `${indent}--hu-${n}: ${v};`);

export function generateQuietCSS(web: TokenMap): string {
  const keys = Object.keys(web);
  if (keys.length === 0) return "";
  for (const k of keys) {
    if (!PATH.test(k)) {
      throw new Error(
        `quiet.tokens.json: unexpected token path "${k}" (expected quiet.light.*, quiet.dark.* or quiet.type.*)`,
      );
    }
    const v = String(web[k]);
    if (v.includes("{")) {
      throw new Error(
        `quiet.tokens.json: ${k} = ${v} contains an unresolved reference; web-only tokens must be literal values`,
      );
    }
  }
  const light = group(web, "quiet.light.");
  const dark = group(web, "quiet.dark.");
  const type = group(web, "quiet.type.");
  const onlyLight = [...light.keys()].filter((n) => !dark.has(n));
  const onlyDark = [...dark.keys()].filter((n) => !light.has(n));
  if (onlyLight.length || onlyDark.length) {
    throw new Error(
      `quiet.tokens.json: light and dark must override the same names; ` +
        `light-only: [${onlyLight.join(", ")}], dark-only: [${onlyDark.join(", ")}]`,
    );
  }
  const out = [
    "/* Quiet Room — web-only layer (design-tokens/quiet.tokens.json).",
    '   Opt in with data-brand="quiet" on <html> or on any container. Put data-theme',
    "   on <html> or on the quiet element itself; data-theme on an intermediate",
    "   wrapper is not supported. */",
  ];
  if (type.size) out.push(`${QUIET_SCOPE} {`, ...decls(type, "  "), "}");
  if (light.size) {
    out.push(
      "@media not ((prefers-contrast: more) or (forced-colors: active)) {",
      `  ${QUIET_SCOPE} {`, ...decls(light, "    "), "  }",
      "  @media (prefers-color-scheme: dark) {",
      `    ${QUIET_DARK_AUTO} {`, ...decls(dark, "      "), "    }",
      "  }",
      `  ${QUIET_DARK_ANCESTOR} {`, ...decls(dark, "    "), "  }",
      `  ${QUIET_LIGHT_ANCESTOR} {`, ...decls(light, "    "), "  }",
      `  ${QUIET_DARK_SELF} {`, ...decls(dark, "    "), "  }",
      `  ${QUIET_LIGHT_SELF} {`, ...decls(light, "    "), "  }",
      "}",
    );
  }
  return out.join("\n");
}
