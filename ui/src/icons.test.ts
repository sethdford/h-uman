import { describe, it, expect } from "vitest";
import { icons } from "./icons.js";

describe("icons", () => {
  const expectedKeys = [
    "grid",
    "message-square",
    "clock",
    "cpu",
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
    expect(icons).toHaveProperty("magnifyingGlass");
    expect(icons).toHaveProperty("refresh");
    expect(icons).toHaveProperty("chevron");
    expect(icons).toHaveProperty("warning");
    expect(icons).toHaveProperty("file-text");
    expect(icons).toHaveProperty("arrow-right");
    expect(icons).toHaveProperty("chat-circle");
    expect(icons).toHaveProperty("monitor");
  });
});
