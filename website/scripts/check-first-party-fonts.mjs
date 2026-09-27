#!/usr/bin/env node
// Fails if built output references Google font hosts or declares a remote
// @font-face source. Run after `npm run build`.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

// fileURLToPath, not .pathname: .pathname keeps %20 for spaces and breaks readdirSync.
const DIST = fileURLToPath(new URL("../dist/", import.meta.url));
const GOOGLE = /fonts\.(googleapis|gstatic)\.com/;
// Absolute (https://host) and protocol-relative (//host) sources are both remote.
const REMOTE_FACE = /@font-face\s*{[^}]*url\(\s*["']?(?:https?:)?\/\//;

function* walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) yield* walk(p);
    else if (/\.(css|html|js)$/.test(name)) yield p;
  }
}

let scanned = 0;
const problems = [];
for (const file of walk(DIST)) {
  scanned++;
  const text = readFileSync(file, "utf-8");
  if (GOOGLE.test(text)) problems.push(`${file}: references a Google font host`);
  if (file.endsWith(".css") && REMOTE_FACE.test(text)) problems.push(`${file}: remote @font-face src`);
}
if (scanned === 0) {
  console.error("check-first-party-fonts: dist/ is empty — run npm run build first");
  process.exit(2);
}
for (const p of problems) console.error(p);
console.log(`check-first-party-fonts: ${scanned} files scanned, ${problems.length} problems`);
process.exit(problems.length ? 1 : 0);
