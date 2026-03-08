#!/usr/bin/env node
/**
 * Dynamic Color Extraction — CLI wrapper
 *
 * Usage:
 *   npx tsx design-tokens/dynamic-color.ts --hex "#7AB648"
 *   npx tsx design-tokens/dynamic-color.ts --hex "#ff6b35" --format css
 *   npx tsx design-tokens/dynamic-color.ts --hex "#7AB648" --format json
 */

import {
  generatePalette,
  generateDynamicColorCSS,
} from "./dynamic-color-lib.js";

function main() {
  const args = process.argv.slice(2);
  let hex = "#7AB648";
  let format = "both";

  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--hex" && args[i + 1]) hex = args[++i];
    if (args[i] === "--format" && args[i + 1]) format = args[++i];
  }

  if (!hex.startsWith("#") || (hex.length !== 7 && hex.length !== 4)) {
    console.error("Invalid hex color. Use format: #rrggbb");
    process.exit(1);
  }

  const palette = generatePalette(hex);

  if (format === "json" || format === "both") {
    console.log(JSON.stringify(palette, null, 2));
  }
  if (format === "css" || format === "both") {
    if (format === "both") console.log("\n---\n");
    console.log(generateDynamicColorCSS(hex));
  }
}

main();
