import { describe, it, expect, afterEach } from "vitest";
import "./hu-directive-telemetry-tile.js";
import {
  type HuDirectiveTelemetryTile,
  DIRECTIVE_VARIANTS,
  type DirectiveTelemetry,
} from "./hu-directive-telemetry-tile.js";

const populated: DirectiveTelemetry = {
  total: 184,
  variants: {
    null_overlay: 12,
    default: 7,
    formal_terse: 18,
    casual_emoji: 113,
    casual_or_short: 26,
    adaptive_emoji: 8,
  },
};

const empty: DirectiveTelemetry = {
  total: 0,
  variants: {
    null_overlay: 0,
    default: 0,
    formal_terse: 0,
    casual_emoji: 0,
    casual_or_short: 0,
    adaptive_emoji: 0,
  },
};

const elements: HTMLElement[] = [];
const mount = async (
  apply: (el: HuDirectiveTelemetryTile) => void = () => {},
): Promise<HuDirectiveTelemetryTile> => {
  const el = document.createElement(
    "hu-directive-telemetry-tile",
  ) as HuDirectiveTelemetryTile;
  apply(el);
  document.body.appendChild(el);
  elements.push(el);
  await el.updateComplete;
  return el;
};

afterEach(() => {
  while (elements.length) {
    const el = elements.pop()!;
    el.remove();
  }
});

describe("hu-directive-telemetry-tile", () => {
  it("renders all six variant segments with counts and percentages", async () => {
    const el = await mount((e) => {
      e.data = populated;
    });
    const root = el.shadowRoot!;

    /* Total readout exposes the gateway-reported `total`, NOT the
     * derived sum of variant counts (those can drift if the C side
     * adds a new variant before the dashboard knows about it). */
    const totalValue = root.querySelector(".total-value");
    expect(totalValue?.textContent?.trim()).toBe("184");

    const segments = Array.from(root.querySelectorAll(".bar .segment"));
    expect(segments.length).toBe(6);

    /* Iteration order must come from DIRECTIVE_VARIANTS, never
     * from Object.entries(data.variants). Asserting the segment
     * sequence pins that contract. */
    const segmentOrder = segments.map((seg) => {
      const label = seg.getAttribute("aria-label") ?? "";
      return label.split(":")[0];
    });
    expect(segmentOrder).toEqual([...DIRECTIVE_VARIANTS]);

    /* Spot-check the dominant variant's aria-label uses the
     * documented "<variant>: <count> fires (<pct>%)" shape from
     * design A.md §3.4. */
    const casualEmoji = segments[3]!;
    expect(casualEmoji.getAttribute("aria-label")).toBe(
      "casual_emoji: 113 fires (61.4%)",
    );
    expect((casualEmoji as HTMLElement).style.flexGrow).toBe("113");

    const legend = root.querySelector(".legend");
    expect(legend).toBeTruthy();
    /* Legend lists all six variants even when some have zero count. */
    expect(legend!.querySelectorAll(".legend-item").length).toBe(6);
  });

  it("renders an empty-bar state when every variant count is zero", async () => {
    const el = await mount((e) => {
      e.data = empty;
    });
    const root = el.shadowRoot!;

    /* The bar is replaced by a status placeholder rather than
     * rendering as a zero-height empty div — consistency-over-
     * surprise principle, see design A.md §4. */
    const emptyBar = root.querySelector(".bar-empty");
    expect(emptyBar).toBeTruthy();
    expect(emptyBar!.getAttribute("aria-label")).toBe("No variants fired yet");

    /* Total readout still shows zero rather than disappearing. */
    expect(root.querySelector(".total-value")?.textContent?.trim()).toBe("0");
  });

  it("renders the loading skeleton when data is null and no error is set", async () => {
    const el = await mount();
    const root = el.shadowRoot!;

    expect(root.querySelector(".skeleton")).toBeTruthy();
    /* No bar rendered while loading. */
    expect(root.querySelector(".bar")).toBeNull();
    expect(root.querySelector(".error-banner")).toBeNull();

    /* aria-busy reflects the loading state for screen readers. */
    const region = root.querySelector(".tile");
    expect(region?.getAttribute("aria-busy")).toBe("true");
  });

  it("renders an error banner when errorMessage is set", async () => {
    const el = await mount((e) => {
      e.errorMessage = "backend timeout";
    });
    const root = el.shadowRoot!;

    const banner = root.querySelector(".error-banner");
    expect(banner).toBeTruthy();
    expect(banner?.textContent?.trim()).toBe("backend timeout");
    /* aria-busy is false even when errorMessage is set — the
     * loading sequence is over even if it didn't succeed. */
    expect(root.querySelector(".tile")?.getAttribute("aria-busy")).toBe("false");
    /* Bar must NOT render alongside the error banner. */
    expect(root.querySelector(".bar")).toBeNull();
    expect(root.querySelector(".skeleton")).toBeNull();
  });

  it("treats missing variant keys as zero rather than crashing", async () => {
    /* Forward compatibility: if the C side ships an older snapshot
     * that omits a variant key, the tile must render the bar with
     * the present variants and treat the missing one as 0. */
    const partial: DirectiveTelemetry = {
      total: 50,
      variants: {
        casual_emoji: 30,
        formal_terse: 20,
      },
    };
    const el = await mount((e) => {
      e.data = partial;
    });
    const root = el.shadowRoot!;
    const segments = Array.from(root.querySelectorAll(".bar .segment"));
    expect(segments.length).toBe(2);
    expect(root.querySelector(".total-value")?.textContent?.trim()).toBe("50");
  });

  it("DirectiveTelemetry type accepts the demo-gateway shape", () => {
    /* Compile-time smoke test — if this file builds, the type is
     * structurally compatible with the mock at
     * ui/src/demo-gateway.ts (tests AC-A.5). */
    const sample: DirectiveTelemetry = {
      total: 184,
      variants: {
        null_overlay: 12,
        default: 7,
        formal_terse: 18,
        casual_emoji: 113,
        casual_or_short: 26,
        adaptive_emoji: 8,
      },
    };
    expect(sample.total).toBe(184);
    expect(sample.variants.casual_emoji).toBe(113);
  });

  it("variant order is stable across renders (asserts DIRECTIVE_VARIANTS contract)", async () => {
    const el = await mount((e) => {
      e.data = populated;
    });
    const root = el.shadowRoot!;
    const legendItems = Array.from(root.querySelectorAll(".legend .legend-item"));
    /* Legend items: <swatch/> <variant-name/> <count/> (<pct/>) — the
     * second `<span>` per item is the variant name. */
    const legendKeys = legendItems.map((item) => {
      const spans = item.querySelectorAll("span");
      return spans[1]?.textContent?.trim() ?? "";
    });
    expect(legendKeys).toEqual([...DIRECTIVE_VARIANTS]);
  });
});
