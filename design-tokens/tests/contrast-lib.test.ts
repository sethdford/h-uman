import { test } from "node:test";
import assert from "node:assert/strict";
import {
  BG_ROLES,
  FILL_PAIRS,
  TEXT_ROLES,
  UI_ROLES,
  UnmeasurableColorError,
  applyBaseline,
  checkBaseContrast,
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

/** The passing fixture re-keyed as base semantic tokens (`<mode>.<name>`). */
function passingBase(): Record<string, string> {
  const m: Record<string, string> = {};
  for (const [k, v] of Object.entries(passingFixture())) m[k.replace(/^quiet\./, "")] = v;
  return m;
}

test("base: a passing palette produces zero failures", () => {
  assert.deepEqual(checkBaseContrast(passingBase()).filter((r) => !r.ok), []);
});

test("base: a role the base theme lacks is a failure, not a skip", () => {
  const shared = passingBase();
  delete shared["light.on-accent"];
  const f = checkBaseContrast(shared).filter((r) => !r.ok);
  assert.deepEqual(f.map((r) => [r.mode, r.fg, r.bg, r.reason]), [
    ["light", "on-accent", "accent", "missing"],
    ["light", "on-accent", "accent-hover", "missing"],
  ]);
});

test("DISCRIMINATES: white on-accent over human.500 (the 2.4:1 regression) fails the base gate", () => {
  const shared = passingBase();
  shared["light.accent"] = "#7ab648";
  shared["light.on-accent"] = "#ffffff";
  const { fresh, known } = applyBaseline(checkBaseContrast(shared), {});
  assert.deepEqual(known, []);
  assert.deepEqual(fresh.map((r) => [r.mode, r.fg, r.bg]), [["light", "on-accent", "accent"]]);
  near(fresh[0].ratio!, 2.4, 0.05);
});

test("baseline: a baselined failure at its recorded ratio is tolerated, not hidden", () => {
  const shared = passingBase();
  shared["dark.text-faint"] = "#5c5549";
  const failures = checkBaseContrast(shared).filter((r) => !r.ok);
  assert.ok(failures.length > 0 && failures.every((r) => r.fg === "text-faint"));
  const baseline = Object.fromEntries(failures.map((r) => [`dark text-faint on ${r.bg}`, Math.round(r.ratio! * 100) / 100]));
  const v = applyBaseline(checkBaseContrast(shared), baseline);
  assert.deepEqual(v.fresh, []);
  assert.equal(v.known.length, failures.length);
  assert.deepEqual(v.fixed, []);
});

test("baseline: a NEW failure beside baselined ones fails the gate", () => {
  const shared = passingBase();
  shared["dark.text-faint"] = "#5c5549";
  const baseline = Object.fromEntries(
    checkBaseContrast(shared).filter((r) => !r.ok).map((r) => [`dark text-faint on ${r.bg}`, Math.round(r.ratio! * 100) / 100]),
  );
  shared["light.warning"] = "#ca8a04";
  const { fresh } = applyBaseline(checkBaseContrast(shared), baseline);
  assert.ok(fresh.length > 0);
  for (const r of fresh) assert.deepEqual([r.mode, r.fg], ["light", "warning"]);
});

test("baseline: a baselined pair that gets worse fails; one that is fixed is reported", () => {
  const shared = passingBase();
  shared["dark.text-faint"] = "#3a352e";
  const v = applyBaseline(checkBaseContrast(shared), { "dark text-faint on bg": 2.5, "light text on bg": 3 });
  const worse = v.fresh.find((r) => r.bg === "bg");
  assert.ok(worse && worse.ratio! < 2.5, "worse-than-recorded pair must be fresh");
  assert.deepEqual(v.fixed, ["light text on bg"]);
});
