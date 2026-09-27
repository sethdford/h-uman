import { test } from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import {
  buildTokensStudio,
  tokensStudioOutPath,
} from "../tokens-studio-lib.js";

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("buildTokensStudio emits the ten Tokens Studio sets", () => {
  assert.deepEqual(Object.keys(buildTokensStudio(DIR)), [
    "base",
    "data-viz",
    "semantic",
    "typography",
    "motion",
    "glass",
    "components",
    "opacity",
    "elevation",
    "breakpoints",
  ]);
});

test("buildTokensStudio resolves every chart.categorical ref to a value", () => {
  const studio = buildTokensStudio(DIR) as {
    "data-viz": { chart: { categorical: Record<string, { $value?: unknown }> } };
  };
  const cat = studio["data-viz"].chart.categorical;
  const entries = Object.entries(cat).filter(([k]) => !k.startsWith("$"));
  assert.ok(entries.length > 0);
  for (const [k, v] of entries) {
    assert.doesNotMatch(String(v.$value), /[{}]/, `categorical.${k} still a ref`);
  }
});

test("tokensStudioOutPath honors --outdir and defaults to docs/", () => {
  assert.equal(
    tokensStudioOutPath(["--outdir", "/tmp/x"]),
    path.join("/tmp/x", "tokens-studio.json"),
  );
  assert.equal(
    tokensStudioOutPath([]),
    path.join(DIR, "..", "docs", "tokens-studio.json"),
  );
});
