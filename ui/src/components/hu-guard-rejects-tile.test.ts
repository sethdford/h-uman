import { describe, it, expect, afterEach } from "vitest";
import "./hu-guard-rejects-tile.js";
import {
  type HuGuardRejectsTile,
  GUARD_REJECT_KEYS,
  type GuardRejectStats,
} from "./hu-guard-rejects-tile.js";

const populated: GuardRejectStats = {
  semantic_leak: 42,
  length_anomaly: 18,
  director_echo: 7,
  persona_pii_echo: 3,
  persona_identity_echo: 2,
};

const empty: GuardRejectStats = {
  semantic_leak: 0,
  length_anomaly: 0,
  director_echo: 0,
  persona_pii_echo: 0,
  persona_identity_echo: 0,
};

const elements: HTMLElement[] = [];
const mount = async (
  apply: (el: HuGuardRejectsTile) => void = () => {},
): Promise<HuGuardRejectsTile> => {
  const el = document.createElement("hu-guard-rejects-tile") as HuGuardRejectsTile;
  apply(el);
  document.body.appendChild(el);
  elements.push(el);
  await el.updateComplete;
  return el;
};

afterEach(() => {
  while (elements.length) {
    elements.pop()!.remove();
  }
});

describe("hu-guard-rejects-tile", () => {
  it("renders stacked segments in GUARD_REJECT_KEYS order", async () => {
    const el = await mount((e) => {
      e.data = populated;
    });
    const root = el.shadowRoot!;
    expect(root.querySelector(".total-value")?.textContent?.trim()).toBe("72");

    const segments = Array.from(root.querySelectorAll(".bar .segment"));
    expect(segments.length).toBe(5);
    const order = segments.map((seg) => (seg.getAttribute("aria-label") ?? "").split(":")[0]);
    expect(order).toEqual([
      "semantic / template",
      "length anomaly",
      "director echo",
      "persona PII",
      "persona identity",
    ]);
  });

  it("shows empty state when all counters are zero", async () => {
    const el = await mount((e) => {
      e.data = empty;
    });
    const root = el.shadowRoot!;
    expect(root.querySelector(".bar-empty")?.textContent).toContain("no rejects");
    expect(root.querySelectorAll(".bar .segment").length).toBe(0);
  });

  it("renders skeleton while loading", async () => {
    const el = await mount();
    expect(el.shadowRoot!.querySelector(".skeleton")).toBeTruthy();
  });

  it("renders error banner when errorMessage is set", async () => {
    const el = await mount((e) => {
      e.errorMessage = "gateway unavailable";
    });
    expect(el.shadowRoot!.querySelector(".error-banner")?.textContent).toContain(
      "gateway unavailable",
    );
  });

  it("exports stable key order matching the C handler", () => {
    expect([...GUARD_REJECT_KEYS]).toEqual([
      "semantic_leak",
      "length_anomaly",
      "director_echo",
      "persona_pii_echo",
      "persona_identity_echo",
    ]);
  });
});
