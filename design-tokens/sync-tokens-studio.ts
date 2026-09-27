#!/usr/bin/env npx tsx
/**
 * Regenerate docs/tokens-studio.json from canonical *.tokens.json sources.
 * Run from repo root: npx tsx design-tokens/sync-tokens-studio.ts [--outdir DIR]
 * The export itself lives in tokens-studio-lib.ts (shared with figma-sync.ts).
 */

import { writeTokensStudio } from "./tokens-studio-lib.js";

console.log("Wrote", writeTokensStudio());
