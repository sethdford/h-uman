#!/usr/bin/env node
/**
 * check-install-matches-formula.mjs
 *
 * Single-source-of-truth drift detector.
 *
 * Reads Formula/human.rb and website/src/data/install.json and asserts that
 * the install one-liner rendered on h-uman.ai is consistent with the formula
 * users will actually `brew tap` and `brew install`. Exits 0 on match,
 * 1 on any drift, with a human-readable diff.
 *
 * Sources of truth parsed from Formula/human.rb:
 *   - `# tap: <org/name>`             (comment-pinned, required)
 *   - `version "<X.Y.Z>"`             (the formula's version keyword)
 *
 * Drift rules:
 *   1. install.json.tap MUST equal the formula's pinned tap comment.
 *   2. install.json.version MUST equal the formula's version keyword.
 *   3. install.json.package MUST equal `<tap>/<formula-name>` where
 *      formula-name is the lowercased class name (here: "human").
 *   4. install.json.command MUST equal:
 *        `brew tap <tap> && brew install <package>`
 *
 * Any rule failing prints a DRIFT line naming the offending field and
 * the expected vs. actual value. The script exits non-zero so the
 * vitest spec and any CI step that invokes it fail by design.
 *
 * Per .claude/rules/tests-that-pin-bugs.md the assertion is on the
 * dangerous outcome (rendered command does not match formula → user
 * runs a wrong `brew install`), not on "command is non-empty".
 */

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// website/scripts/ → repo root is two levels up.
const REPO_ROOT = resolve(__dirname, "..", "..");
const FORMULA_PATH = resolve(REPO_ROOT, "Formula", "human.rb");
const INSTALL_JSON_PATH = resolve(
  REPO_ROOT,
  "website",
  "src",
  "data",
  "install.json",
);

function fail(msg) {
  console.error(`DRIFT: ${msg}`);
  process.exit(1);
}

function loadFormula() {
  let text;
  try {
    text = readFileSync(FORMULA_PATH, "utf8");
  } catch (err) {
    fail(`cannot read ${FORMULA_PATH}: ${err.message}`);
  }

  // Tap is pinned in a comment line `# tap: <org>/<name>`.
  const tapMatch = text.match(/^#\s*tap:\s*([a-z0-9_-]+\/[a-z0-9_-]+)\s*$/m);
  if (!tapMatch) {
    fail(
      `Formula/human.rb is missing required '# tap: <org>/<name>' comment line. ` +
        `This pin is the single source of truth for the install one-liner.`,
    );
  }
  const tap = tapMatch[1];

  // Version is a `version "X.Y.Z"` line. Allow any non-empty version
  // string so the parser does not pin a semver shape that future
  // pre-release tags (e.g. 0.5.0-rc.1) would trip on.
  const versionMatch = text.match(/^\s*version\s+"([^"]+)"\s*$/m);
  if (!versionMatch) {
    fail(`Formula/human.rb is missing 'version "..."' keyword.`);
  }
  const version = versionMatch[1];

  // Formula class name is `class Human < Formula` → "human" lowercased.
  const classMatch = text.match(/^\s*class\s+([A-Z][A-Za-z0-9]*)\s+<\s+Formula\b/m);
  if (!classMatch) {
    fail(`Formula/human.rb is missing 'class <Name> < Formula' declaration.`);
  }
  const formulaName = classMatch[1].toLowerCase();

  return { tap, version, formulaName };
}

function loadInstallJson() {
  let raw;
  try {
    raw = readFileSync(INSTALL_JSON_PATH, "utf8");
  } catch (err) {
    fail(`cannot read ${INSTALL_JSON_PATH}: ${err.message}`);
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (err) {
    fail(`install.json is not valid JSON: ${err.message}`);
  }
  for (const k of ["tap", "package", "version", "command"]) {
    if (typeof parsed[k] !== "string" || parsed[k].length === 0) {
      fail(`install.json.${k} must be a non-empty string`);
    }
  }
  return parsed;
}

function checkDrift({ formula, install }) {
  const expectedPackage = `${formula.tap}/${formula.formulaName}`;
  const expectedCommand = `brew tap ${formula.tap} && brew install ${expectedPackage}`;

  if (install.tap !== formula.tap) {
    fail(
      `install.json.tap=${install.tap} but Formula/human.rb pins tap=${formula.tap}`,
    );
  }
  if (install.version !== formula.version) {
    fail(
      `install.json.version=${install.version} but Formula/human.rb version=${formula.version}`,
    );
  }
  if (install.package !== expectedPackage) {
    fail(
      `install.json.package=${install.package} but expected ${expectedPackage} ` +
        `(derived from formula tap + class name)`,
    );
  }
  if (install.command !== expectedCommand) {
    fail(
      `install.json.command mismatch.\n` +
        `  expected: ${expectedCommand}\n` +
        `  actual:   ${install.command}`,
    );
  }
}

function main() {
  const formula = loadFormula();
  const install = loadInstallJson();
  checkDrift({ formula, install });
  console.log(
    `OK: install.json matches Formula/human.rb (tap=${formula.tap}, version=${formula.version})`,
  );
  process.exit(0);
}

main();
