/**
 * install-section.test.ts
 *
 * Pins the install one-liner displayed on h-uman.ai against
 * `Formula/human.rb`. Failures here mean the marketing site is about
 * to ship a `brew install` command that does not match the formula
 * users will actually receive.
 *
 * See `sprints/sprint-9/designs/US-9.5.md` and
 * `.claude/rules/tests-that-pin-bugs.md` for the rationale on assertion
 * phrasing — every assertion below is on the dangerous outcome
 * (drift, hard-coded semver) rather than on "value is non-empty".
 */

import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// website/tests/ → website/ → repo root.
const WEBSITE_DIR = resolve(__dirname, "..");
const REPO_ROOT = resolve(WEBSITE_DIR, "..");
const INSTALL_JSON_PATH = resolve(
  WEBSITE_DIR,
  "src",
  "data",
  "install.json",
);
const COMPONENT_PATH = resolve(
  WEBSITE_DIR,
  "src",
  "components",
  "InstallSection.astro",
);
const CHECK_SCRIPT = resolve(
  WEBSITE_DIR,
  "scripts",
  "check-install-matches-formula.mjs",
);

interface VerifiedState {
  ok: boolean | null;
  release_tag: string | null;
  checked_at: string | null;
  ci_run_url: string | null;
}

interface InstallData {
  tap: string;
  package: string;
  version: string;
  command: string;
  verified?: {
    macos_arm64?: VerifiedState;
  };
}

const installData: InstallData = JSON.parse(
  readFileSync(INSTALL_JSON_PATH, "utf8"),
);

describe("US-9.5: website install one-liner + drift detection", () => {
  // ─── Test A: install.json schema sanity ────────────────────────
  it("install.json has required non-empty string fields", () => {
    expect(typeof installData.tap).toBe("string");
    expect(installData.tap.length).toBeGreaterThan(0);
    expect(typeof installData.package).toBe("string");
    expect(installData.package.length).toBeGreaterThan(0);
    expect(typeof installData.version).toBe("string");
    expect(installData.version.length).toBeGreaterThan(0);
    expect(typeof installData.command).toBe("string");
    expect(installData.command.length).toBeGreaterThan(0);
  });

  it("install.json verified.macos_arm64.ok is a boolean (badge truth surface)", () => {
    // The badge displayed to users binds to this field. It must be a
    // strict boolean — a null or string here would render an
    // ambiguous badge state which is the lie we are guarding against.
    const verified = installData.verified?.macos_arm64;
    expect(verified, "verified.macos_arm64 must be present").toBeDefined();
    expect(typeof verified!.ok).toBe("boolean");
  });

  // ─── Test B: drift detector (the load-bearing assertion) ───────
  //
  // FAIL = the rendered one-liner on h-uman.ai would not match the
  // formula users actually `brew tap`/`brew install` against. Per
  // `.claude/rules/tests-that-pin-bugs.md` the assertion is on the
  // dangerous outcome, not on a benign property.
  it("install_command_matches_formula_or_drift_is_detected", () => {
    const result = spawnSync(
      process.execPath,
      [CHECK_SCRIPT],
      {
        cwd: REPO_ROOT,
        encoding: "utf8",
      },
    );
    if (result.status !== 0) {
      // Surface the script's stderr in the test report so a CI run
      // failing this test names the offending field, not just exit 1.
      const stderr = result.stderr ?? "";
      const stdout = result.stdout ?? "";
      throw new Error(
        `install command drifted from Formula/human.rb (exit=${result.status}):\n` +
          `STDERR:\n${stderr}\nSTDOUT:\n${stdout}`,
      );
    }
    expect(
      result.status,
      "install command must match Formula/human.rb",
    ).toBe(0);
  });

  // ─── Test C: pin AC-9.5.1 literal ──────────────────────────────
  it("install.json.command matches the AC-9.5.1 literal", () => {
    expect(installData.command).toBe(
      "brew tap humanlabs/human && brew install humanlabs/human/human",
    );
  });

  // ─── Test D: no hard-coded semver in component source ──────────
  //
  // AC-9.5.2 requires the displayed version to come from
  // `install.json`, not from any string baked into the Astro template.
  // A future implementer could "helpfully" inline `"0.5.0"` and this
  // test will catch it.
  it("InstallSection.astro contains no hard-coded semver string literal", () => {
    const src = readFileSync(COMPONENT_PATH, "utf8");
    // Strip line comments and block comments so doc strings that
    // mention version examples in prose don't trip the guard.
    const stripped = src
      .replace(/\/\*[\s\S]*?\*\//g, "")
      .replace(/(^|[^:])\/\/[^\n]*/g, "$1");
    const semverLiteral = /["']\d+\.\d+\.\d+["']/;
    const matches = stripped.match(semverLiteral);
    expect(
      matches,
      "InstallSection.astro must not contain a hard-coded semver string " +
        "literal — version must come from install.json (AC-9.5.2)",
    ).toBeNull();
  });
});
