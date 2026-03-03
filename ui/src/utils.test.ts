import { describe, it, expect } from "vitest";
import { formatDate, formatRelative, SESSION_KEY_DEFAULT, EVENT_NAMES } from "./utils.js";

describe("formatDate", () => {
  it("returns dash for null", () => {
    expect(formatDate(null)).toBe("—");
  });

  it("returns dash for undefined", () => {
    expect(formatDate(undefined)).toBe("—");
  });

  it("formats a unix timestamp in seconds", () => {
    const result = formatDate(1700000000);
    expect(result).toBeTruthy();
    expect(result).not.toBe("—");
  });

  it("formats a unix timestamp in milliseconds", () => {
    const result = formatDate(1700000000000);
    expect(result).toBeTruthy();
    expect(result).not.toBe("—");
  });

  it("formats a date string", () => {
    const result = formatDate("2024-01-15T12:00:00Z");
    expect(result).toBeTruthy();
    expect(result).not.toBe("—");
  });

  it("returns string for invalid input", () => {
    const result = formatDate("not-a-date");
    expect(typeof result).toBe("string");
  });
});

describe("formatRelative", () => {
  it("returns dash for null", () => {
    expect(formatRelative(null)).toBe("—");
  });

  it("returns dash for undefined", () => {
    expect(formatRelative(undefined)).toBe("—");
  });

  it('returns "just now" for recent timestamps', () => {
    const recent = Date.now() - 5000;
    expect(formatRelative(recent)).toBe("just now");
  });

  it('returns "Xm ago" for minutes', () => {
    const fiveMinAgo = Date.now() - 5 * 60 * 1000;
    expect(formatRelative(fiveMinAgo)).toBe("5m ago");
  });

  it('returns "Xh ago" for hours', () => {
    const threeHoursAgo = Date.now() - 3 * 3600 * 1000;
    expect(formatRelative(threeHoursAgo)).toBe("3h ago");
  });

  it('returns "Xd ago" for days', () => {
    const twoDaysAgo = Date.now() - 2 * 86400 * 1000;
    expect(formatRelative(twoDaysAgo)).toBe("2d ago");
  });
});

describe("constants", () => {
  it("SESSION_KEY_DEFAULT is 'default'", () => {
    expect(SESSION_KEY_DEFAULT).toBe("default");
  });

  it("EVENT_NAMES has expected values", () => {
    expect(EVENT_NAMES.CHAT).toBe("chat");
    expect(EVENT_NAMES.TOOL_CALL).toBe("agent.tool");
    expect(EVENT_NAMES.ERROR).toBe("error");
    expect(EVENT_NAMES.HEALTH).toBe("health");
  });
});
