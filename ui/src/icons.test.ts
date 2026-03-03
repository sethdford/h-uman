import { describe, it, expect } from "vitest";
import { icons } from "./icons.js";

describe("icons", () => {
  const expectedKeys = [
    "grid",
    "message-square",
    "clock",
    "cpu",
    "box",
    "mic",
    "wrench",
    "radio",
    "puzzle",
    "timer",
    "settings",
    "server",
    "bar-chart",
    "terminal",
  ];

  it("has all navigation icons", () => {
    for (const key of expectedKeys) {
      expect(icons).toHaveProperty(key);
    }
  });

  it("every icon value is truthy", () => {
    for (const [key, value] of Object.entries(icons)) {
      expect(value, `icon "${key}" should be truthy`).toBeTruthy();
    }
  });

  it("has at least 14 icons", () => {
    expect(Object.keys(icons).length).toBeGreaterThanOrEqual(14);
  });

  it("includes utility icons", () => {
    expect(icons).toHaveProperty("search");
    expect(icons).toHaveProperty("refresh");
    expect(icons).toHaveProperty("chevron");
  });
});
