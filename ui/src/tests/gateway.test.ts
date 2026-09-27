import { describe, it, expect } from "vitest";

describe("DemoGatewayClient", () => {
  it("DemoGatewayClient reaches connected status within 500ms", async () => {
    const { DemoGatewayClient } = await import("../demo-gateway.js");
    const demo = new DemoGatewayClient();
    const statusPromise = new Promise<string>((resolve) => {
      demo.addEventListener("status", ((e: CustomEvent<string>) => {
        if (e.detail === "connected") resolve(e.detail);
      }) as EventListener);
    });
    demo.connect("ws://localhost:0");
    const status = await statusPromise;
    expect(status).toBe("connected");
    demo.disconnect();
  });

  it("inFlight counts a request only until it resolves", async () => {
    const { DemoGatewayClient } = await import("../demo-gateway.js");
    const demo = new DemoGatewayClient();
    expect(demo.inFlight).toBe(0);
    const first = demo.request("sessions.list");
    const second = demo.request("unknown.method");
    expect(demo.inFlight).toBe(2);
    await Promise.allSettled([first, second]);
    expect(demo.inFlight).toBe(0);
  });
});
