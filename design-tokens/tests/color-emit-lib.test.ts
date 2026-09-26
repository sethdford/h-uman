import { test } from "node:test";
import assert from "node:assert/strict";
import {
  UnsupportedColorError,
  colorToKotlin,
  formatSwiftColor,
  hexToKotlin,
  hexToSwift,
  isColorLike,
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

for (const good of [
  "#7AB648",
  "rgba(0, 0, 0, 0.5)",
  "rgb(1, 2, 3)",
  "oklch(50% 0.13 135)",
  "color(display-p3 0.5 0.7 0.3)",
  "hsl(120 50% 50%)",
]) {
  test(`isColorLike is true for ${good}`, () => {
    assert.equal(isColorLike(good), true);
  });
}

for (const notColor of [
  "2px",
  "0.5rem",
  "45deg",
  "50%",
  "linear-gradient(red, blue)",
  "1.5",
]) {
  test(`isColorLike is false for ${notColor}`, () => {
    assert.equal(isColorLike(notColor), false);
  });
}
