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
});
