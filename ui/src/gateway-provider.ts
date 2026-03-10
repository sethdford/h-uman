import type { GatewayClient } from "./gateway.js";

export const GATEWAY_CHANGED = "hu-gateway-changed";

export interface GatewayChangedDetail {
  previous: GatewayClient | null;
  current: GatewayClient;
}

let _gateway: GatewayClient | null = null;

export function setGateway(gw: GatewayClient): void {
  const prev = _gateway;
  _gateway = gw;
  if (prev !== gw) {
    document.dispatchEvent(
      new CustomEvent<GatewayChangedDetail>(GATEWAY_CHANGED, {
        detail: { previous: prev, current: gw },
      }),
    );
  }
}

export function getGateway(): GatewayClient | null {
  return _gateway;
}
