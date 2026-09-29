import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { setGateway } from "../gateway-provider.js";
import type { GatewayClient } from "../gateway.js";
import "./overview-view.js";

/** Records every request; `update.check` answers like cp_admin_update_check
 * with auto_update="off": disabled unless { force: true }. */
class FakeGateway extends EventTarget {
  status = "connected";
  calls: Array<{ method: string; params: Record<string, unknown> }> = [];

  async request<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
    this.calls.push({ method, params });
    if (method === "update.check") {
      return (
        params.force === true
          ? { current: "1.0.0", latest: "1.1.0", available: true }
          : { current: "1.0.0", available: false, disabled: true }
      ) as T;
    }
    return {} as T;
  }

  updateChecks() {
    return this.calls.filter((c) => c.method === "update.check");
  }
}

type OverviewEl = HTMLElement & { updateComplete: Promise<boolean> };

async function mountWith(gw: FakeGateway): Promise<OverviewEl> {
  setGateway(gw as unknown as GatewayClient);
  const el = document.createElement("hu-overview-view") as OverviewEl;
  document.body.appendChild(el);
  await vi.waitFor(() => {
    expect(el.shadowRoot?.querySelector(".hero-meta")).toBeTruthy();
  });
  await el.updateComplete;
  return el;
}

describe("hu-overview-view update check", () => {
  let el: OverviewEl | null = null;
  // The hero's version line (where update status lives) renders once onboarded.
  beforeEach(() => localStorage.setItem("hu-onboarded", "true"));
  afterEach(() => {
    el?.remove();
    el = null;
    localStorage.removeItem("hu-onboarded");
  });

  it("loads without forcing a network check and shows the off state", async () => {
    const gw = new FakeGateway();
    el = await mountWith(gw);
    const checks = gw.updateChecks();
    expect(checks.length).toBeGreaterThanOrEqual(1);
    expect(checks.every((c) => c.params.force !== true)).toBe(true);
    const meta = el.shadowRoot!.querySelector(".hero-meta")!;
    expect(meta.textContent).toContain("Update checks are off");
    expect(meta.querySelector("button.update-link")?.textContent?.trim()).toBe("Check now");
    expect(meta.querySelector("a.update-link")).toBeNull();
  });

  it("forces a check only when Check now is clicked", async () => {
    const gw = new FakeGateway();
    el = await mountWith(gw);
    const before = gw.updateChecks().length;
    (el.shadowRoot!.querySelector("button.update-link") as HTMLButtonElement).click();
    await vi.waitFor(() => {
      expect(el!.shadowRoot!.querySelector("a.update-link")).toBeTruthy();
    });
    const after = gw.updateChecks();
    expect(after.length).toBe(before + 1);
    expect(after[after.length - 1].params).toEqual({ force: true });
    const link = el.shadowRoot!.querySelector("a.update-link")!;
    expect(link.textContent?.trim()).toBe("Update to 1.1.0");
    expect(el.shadowRoot!.querySelector("button.update-link")).toBeNull();
  });
});
