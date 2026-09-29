#!/usr/bin/env node
/**
 * Measures the Quiet Room layer and the base semantic themes. Exit 0 = every
 * pair passes (base pairs listed in contrast-baseline.json may keep failing at
 * or above their recorded ratio); 1 = a pair fails, a baselined pair got worse
 * or now passes (stale entries must be deleted), or a color is out of gamut;
 * 2 = nothing to measure (no web tokens) — distinct from 0 so a missing layer
 * can never read as "passed".
 */
import * as fs from "fs";
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
import {
  applyBaseline,
  checkBaseContrast,
  checkQuietContrast,
  checkQuietGamut,
  pairKey,
  type ContrastBaseline,
  type PairResult,
} from "./contrast-lib.js";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const BASELINE_FILE = path.join(DIR, "contrast-baseline.json");
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

function readBaseline(): ContrastBaseline {
  const base = JSON.parse(fs.readFileSync(BASELINE_FILE, "utf8")).base;
  const valid =
    base !== null &&
    typeof base === "object" &&
    !Array.isArray(base) &&
    Object.values(base).every((v) => v === null || typeof v === "number");
  if (!valid) throw new Error(`${BASELINE_FILE}: "base" must map pair keys to a ratio or null`);
  return base;
}

const got = (r: PairResult) => (r.ratio === null ? r.reason : `${r.ratio.toFixed(2)}:1`);
const line = (tag: string, layer: string, r: PairResult) =>
  `${tag} ${layer} ${r.mode.padEnd(5)} ${r.fg} on ${r.bg}: ${got(r)} (need ${r.need}:1)`;

const quiet = checkQuietContrast(sharedMap, webMap);
const base = checkBaseContrast(sharedMap);
const baseline = readBaseline();
const verdict = applyBaseline(base, baseline);
const gamut = checkQuietGamut(webMap);
const quietFailed = quiet.filter((r) => !r.ok);

for (const r of verdict.known) console.log(line("KNOWN", "base ", r));
for (const k of verdict.fixed) {
  console.error(`FAIL base  ${k} is baselined but no longer fails — delete it from contrast-baseline.json`);
}
for (const r of quietFailed) console.error(line("FAIL", "quiet", r));
for (const r of verdict.fresh) {
  const was = baseline[pairKey(r)];
  const note = was === undefined ? "" : ` — worse than its baselined ${was === null ? "unmeasurable" : `${was}:1`}`;
  console.error(line("FAIL", "base ", r) + note);
}
for (const g of gamut) console.error(`FAIL gamut ${g}`);

const all = [...quiet, ...base];
// No measurable pair at all still reports every failure above and exits 1;
// only the "tightest" clause has nothing to name.
const worst = all
  .filter((r) => r.ratio !== null)
  .reduce<PairResult | null>((a, b) => (a && a.ratio! - a.need < b.ratio! - b.need ? a : b), null);
const tightest = worst
  ? `tightest ${worst.mode} ${worst.fg} on ${worst.bg} ${worst.ratio!.toFixed(2)}:1 (need ${worst.need})`
  : "no pair was measurable";
const failed = quietFailed.length + verdict.fresh.length + verdict.fixed.length;
console.log(
  `check-contrast: ${quiet.length} quiet + ${base.length} base pairs measured, ${failed} failed, ` +
    `${verdict.known.length} base pairs baselined, ${gamut.length} out of gamut; ` +
    tightest,
);
process.exit(failed || gamut.length ? 1 : 0);
