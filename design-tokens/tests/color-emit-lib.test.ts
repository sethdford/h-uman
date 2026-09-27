import { test } from "node:test";
import assert from "node:assert/strict";
import {
  UnsupportedColorError,
  colorToKotlin,
  formatSwiftColor,
  hexToKotlin,
  hexToSwift,
  nativeColorEntries,
  terminalRGB,
} from "../color-emit-lib.js";

test("hex converts exactly as before", () => {
  assert.equal(hexToSwift("#7AB648"), "0x7AB648");
  assert.equal(hexToKotlin("#7ab648"), "0xFF7AB648");
  assert.equal(formatSwiftColor("#7ab648"), "Color(hex: 0x7AB648)");
});

test("rgba converts exactly as before", () => {
  assert.equal(
    formatSwiftColor("rgba(255, 255, 255, 0.08)"),
    "Color(red: 1, green: 1, blue: 1, opacity: 0.08)",
  );
  assert.equal(colorToKotlin("rgba(0, 0, 0, 0.5)"), "0x80000000");
});

test("rgb() without alpha is converted, not blackened", () => {
  assert.equal(colorToKotlin("rgb(122, 182, 72)"), "0xFF7AB648");
});

for (const bad of [
  "oklch(50% 0.13 135)",
  "color(display-p3 0.5 0.7 0.3)",
  "#fff",
  "#7AB648CC",
  "linear-gradient(red, blue)",
]) {
  test(`throws UnsupportedColorError for ${bad}`, () => {
    assert.throws(() => formatSwiftColor(bad), UnsupportedColorError);
    assert.throws(() => colorToKotlin(bad), UnsupportedColorError);
  });
}

test("the error names the offending value", () => {
  assert.throws(() => formatSwiftColor("oklch(50% 0.13 135)"), /oklch\(50% 0\.13 135\)/);
});

test("nativeColorEntries selects $type color under the prefix, sorted", () => {
  const tokens = {
    "light.b": "rgba(0, 0, 0, 0.5)",
    "light.a": "#7AB648",
    "light.gap": "2px",
    "dark.a": "#000000",
  };
  const types = {
    "light.b": "color",
    "light.a": "color",
    "light.gap": "dimension",
    "dark.a": "color",
  };
  assert.deepEqual(nativeColorEntries(tokens, types, "light."), [
    ["light.a", "#7AB648"],
    ["light.b", "rgba(0, 0, 0, 0.5)"],
  ]);
});

test("nativeColorEntries skips a non-color $type even when it looks like a color", () => {
  assert.deepEqual(
    nativeColorEntries({ "light.x": "#7AB648" }, { "light.x": "string" }, "light."),
    [],
  );
});

for (const bad of [
  "transparent",
  "white",
  "currentColor",
  "color-mix(in srgb, #fff 50%, #000)",
  "light-dark(#fff, #000)",
  "var(--hu-accent)",
  "{light.missing}",
  "oklch(50% 0.13 135)",
]) {
  test(`nativeColorEntries refuses a color-typed ${bad}, naming the token path`, () => {
    assert.throws(
      () => nativeColorEntries({ "light.zz": bad }, { "light.zz": "color" }, "light."),
      (e: unknown) =>
        e instanceof UnsupportedColorError &&
        e.message.includes("light.zz") &&
        e.message.includes(JSON.stringify(bad)),
    );
  });
}

test("nativeColorEntries refuses a color-typed number", () => {
  assert.throws(
    () => nativeColorEntries({ "chart.n": 3 }, { "chart.n": "color" }, "chart."),
    /chart\.n/,
  );
});

test("terminalRGB parses #RRGGBB", () => {
  assert.deepEqual(terminalRGB("dark.bg", "#7AB648"), [122, 182, 72]);
});

for (const bad of ["rgba(0, 0, 0, 0.5)", "transparent", "#fff", undefined]) {
  test(`terminalRGB refuses ${String(bad)}, naming the token path`, () => {
    assert.throws(
      () => terminalRGB("dark.bg-overlay", bad),
      (e: unknown) =>
        e instanceof UnsupportedColorError && e.message.includes("dark.bg-overlay"),
    );
  });
}
