#!/usr/bin/env node
/**
 * Measures the Quiet Room layer. Exit 0 = every pair passes; 1 = a pair
 * fails or a color is out of gamut; 2 = nothing to measure (no web tokens) —
 * distinct from 0 so a missing layer can never read as "passed".
 */
import * as path from "path";
import { fileURLToPath } from "url";
import {
  TOKEN_FILES,
  collectTokens,
  partitionByPlatform,
  readTokenSources,
  resolveRefs,
  type TokenMap,
} from "./token-lib.js";
import { checkQuietContrast, checkQuietGamut } from "./contrast-lib.js";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const { shared, web } = partitionByPlatform(readTokenSources(DIR, TOKEN_FILES));
let sharedMap: TokenMap = {};
for (const { data } of shared) sharedMap = { ...sharedMap, ...collectTokens(data) };
sharedMap = resolveRefs(sharedMap);
let webMap: TokenMap = {};
for (const { data } of web) webMap = { ...webMap, ...collectTokens(data) };

if (Object.keys(webMap).length === 0) {
  console.error("check-contrast: no web-only tokens found — nothing measured");
  process.exit(2);
}

const results = checkQuietContrast(sharedMap, webMap);
const gamut = checkQuietGamut(webMap);
const failed = results.filter((r) => !r.ok);
for (const f of failed) {
  const got = f.ratio === null ? f.reason : `${f.ratio.toFixed(2)}:1`;
  console.error(`FAIL ${f.mode.padEnd(5)} ${f.fg} on ${f.bg}: ${got} (need ${f.need}:1)`);
}
for (const g of gamut) console.error(`FAIL gamut ${g}`);
const worst = results
  .filter((r) => r.ratio !== null)
  .reduce((a, b) => (a.ratio! - a.need < b.ratio! - b.need ? a : b));
console.log(
  `check-contrast: ${results.length} pairs measured, ${failed.length} failed, ` +
    `${gamut.length} out of gamut; tightest ${worst.mode} ${worst.fg} on ${worst.bg} ` +
    `${worst.ratio!.toFixed(2)}:1 (need ${worst.need})`,
);
process.exit(failed.length || gamut.length ? 1 : 0);
