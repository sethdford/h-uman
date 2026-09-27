import { test } from "node:test";
import assert from "node:assert/strict";
import {
  BG_ROLES,
  FILL_PAIRS,
  TEXT_ROLES,
  UI_ROLES,
  UnmeasurableColorError,
  checkQuietContrast,
  checkQuietGamut,
  contrastRatio,
  isInGamut,
  oklchToLinear,
  parseOpaqueColor,
} from "../contrast-lib.js";

const near = (a: number, b: number, tol = 0.01) =>
  assert.ok(Math.abs(a - b) <= tol, `${a} not within ${tol} of ${b}`);

test("black on white is 21:1", () => near(contrastRatio("#000000", "#FFFFFF"), 21));

test("brand green on paper measures 2.25:1 (why the role split exists)", () =>
  near(contrastRatio("oklch(71.5% 0.155 131)", "oklch(97.5% 0.010 95)"), 2.25));

test("accent text on paper measures 5.33:1", () =>
  near(contrastRatio("oklch(50% 0.130 135)", "oklch(97.5% 0.010 95)"), 5.33));

test("rgb() and hex agree", () =>
  near(contrastRatio("rgb(122, 182, 72)", "#FFFFFF"), contrastRatio("#7AB648", "#FFFFFF"), 1e-9));

test("translucent and non-color values are unmeasurable, not guessed", () => {
  assert.throws(() => parseOpaqueColor("rgba(0, 0, 0, 0.5)"), UnmeasurableColorError);
  assert.throws(() => parseOpaqueColor("linear-gradient(red, blue)"), UnmeasurableColorError);
  assert.throws(() => parseOpaqueColor("color(display-p3 0.4 0.6 0.2)"), UnmeasurableColorError);
});

test("gamut: warning at C 0.120 is outside sRGB, at C 0.105 inside", () => {
  assert.equal(isInGamut(oklchToLinear(0.53, 0.12, 65)), false);
  assert.equal(isInGamut(oklchToLinear(0.53, 0.105, 65)), true);
});

/** A web map where every role is set to a known-passing value in both modes. */
function passingFixture(): Record<string, string> {
  const m: Record<string, string> = {};
  const set = (mode: string, names: readonly string[], v: string) => {
    for (const n of names) m[`quiet.${mode}.${n}`] = v;
  };
  set("light", BG_ROLES, "oklch(97.5% 0.010 95)");
  set("light", [...TEXT_ROLES, ...UI_ROLES, "accent", "accent-hover"], "oklch(23% 0.020 150)");
  set("light", ["on-accent"], "oklch(99.5% 0.004 95)");
  set("dark", BG_ROLES, "oklch(17% 0.012 150)");
  set("dark", [...TEXT_ROLES, ...UI_ROLES, "accent", "accent-hover"], "oklch(95% 0.010 120)");
  set("dark", ["on-accent"], "oklch(17% 0.012 150)");
  return m;
}

test("a passing palette produces zero failures (no false positives)", () => {
  const results = checkQuietContrast({}, passingFixture());
  const expected =
    2 * (TEXT_ROLES.length * BG_ROLES.length + UI_ROLES.length * BG_ROLES.length + FILL_PAIRS.length);
  assert.equal(results.length, expected);
  assert.deepEqual(results.filter((r) => !r.ok), []);
});

test("DISCRIMINATES: brand green as accent-text fails, and only for that reason", () => {
  const web = passingFixture();
  web["quiet.light.accent-text"] = "oklch(71.5% 0.155 131)";
  const failures = checkQuietContrast({}, web).filter((r) => !r.ok);
  assert.ok(failures.length > 0, "expected failures");
  for (const f of failures) {
    assert.equal(f.mode, "light");
    assert.equal(f.fg, "accent-text");
    assert.ok(f.ratio !== null && f.ratio < 3, `ratio ${f.ratio}`);
  }
});

test("inherited values are measured: a failing shared token is caught", () => {
  const web = passingFixture();
  delete web["quiet.light.warning"];
  const failures = checkQuietContrast({ "light.warning": "#ca8a04" }, web).filter((r) => !r.ok);
  assert.ok(failures.some((f) => f.fg === "warning" && f.ratio !== null && f.ratio < 3));
});

test("a role defined nowhere is a failure, not a skip", () => {
  const web = passingFixture();
  delete web["quiet.dark.text-faint"];
  const f = checkQuietContrast({}, web).filter((r) => !r.ok);
  assert.ok(f.some((r) => r.fg === "text-faint" && r.mode === "dark" && r.reason === "missing"));
});

test("gamut check names out-of-gamut web colors", () => {
  assert.deepEqual(checkQuietGamut({ "quiet.light.warning": "oklch(53% 0.120 65)" }), [
    "quiet.light.warning = oklch(53% 0.120 65) is outside the sRGB gamut",
  ]);
  assert.deepEqual(checkQuietGamut({ "quiet.light.warning": "oklch(53% 0.105 65)" }), []);
});
