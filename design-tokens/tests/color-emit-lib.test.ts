import { test } from "node:test";
import assert from "node:assert/strict";
import {
  UnsupportedColorError,
  colorToKotlin,
  formatSwiftColor,
  hexToKotlin,
  hexToSwift,
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
