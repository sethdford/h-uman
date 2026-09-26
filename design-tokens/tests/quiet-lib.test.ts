import { test } from "node:test";
import assert from "node:assert/strict";
import {
  QUIET_DARK_ANCESTOR,
  QUIET_DARK_AUTO,
  QUIET_DARK_SELF,
  QUIET_LIGHT_ANCESTOR,
  QUIET_LIGHT_SELF,
  QUIET_SCOPE,
  generateQuietCSS,
} from "../quiet-lib.js";

const MIN = {
  "quiet.type.font-display": "\"Newsreader Variable\", serif",
  "quiet.light.bg": "oklch(97.5% 0.010 95)",
  "quiet.dark.bg": "oklch(17% 0.012 150)",
};

test("no web tokens → no CSS (build unaffected)", () => {
  assert.equal(generateQuietCSS({}), "");
});

test("emits every selector, the contrast guard, and both modes", () => {
  const css = generateQuietCSS(MIN);
  for (const sel of [
    QUIET_SCOPE, QUIET_DARK_AUTO, QUIET_DARK_ANCESTOR, QUIET_LIGHT_ANCESTOR, QUIET_DARK_SELF, QUIET_LIGHT_SELF,
  ]) {
    assert.ok(css.includes(sel), `missing selector ${sel}`);
  }
  assert.ok(css.includes("@media not ((prefers-contrast: more) or (forced-colors: active))"));
  assert.ok(css.includes("@media (prefers-color-scheme: dark)"));
  assert.ok(css.includes("--hu-font-display: \"Newsreader Variable\", serif;"));
  assert.equal(css.match(/--hu-bg: oklch\(97\.5% 0\.010 95\);/g)?.length, 3); // scope + light ancestor + light self
  assert.equal(css.match(/--hu-bg: oklch\(17% 0\.012 150\);/g)?.length, 3); // auto + dark ancestor + dark self
});

test("self forms are emitted after ancestor forms (own data-theme wins)", () => {
  const css = generateQuietCSS(MIN);
  const lastAncestor = Math.max(css.indexOf(QUIET_DARK_ANCESTOR + " {"), css.indexOf(QUIET_LIGHT_ANCESTOR + " {"));
  const firstSelf = Math.min(css.indexOf(QUIET_DARK_SELF + " {"), css.indexOf(QUIET_LIGHT_SELF + " {"));
  assert.ok(lastAncestor > 0 && firstSelf > lastAncestor);
});

test("never declares on bare :root (no leakage into non-quiet pages)", () => {
  assert.doesNotMatch(generateQuietCSS(MIN), /^\s*:root\s*\{/m);
});

test("colors are guarded by prefers-contrast and forced-colors; typography is not", () => {
  const css = generateQuietCSS(MIN);
  const guard = css.indexOf("@media not ((prefers-contrast: more) or (forced-colors: active))");
  assert.ok(guard > 0, "the combined contrast/forced-colors guard must be emitted");
  assert.ok(css.indexOf("--hu-font-display") < guard, "type must precede the contrast guard");
  assert.ok(css.indexOf("--hu-bg") > guard, "colors must sit inside the contrast guard");
});

test("throws on unresolved reference", () => {
  assert.throws(
    () => generateQuietCSS({ ...MIN, "quiet.light.text": "{light.text}" }),
    /quiet\.light\.text.*unresolved reference/,
  );
});

test("throws on light/dark asymmetry, naming the token", () => {
  assert.throws(
    () => generateQuietCSS({ ...MIN, "quiet.light.warning": "oklch(53% 0.105 65)" }),
    /light-only: \[warning\]/,
  );
});

test("throws on a path outside quiet.{light,dark,type}", () => {
  assert.throws(() => generateQuietCSS({ ...MIN, "quiet.spacing.x": "1rem" }), /unexpected token path "quiet\.spacing\.x"/);
});
