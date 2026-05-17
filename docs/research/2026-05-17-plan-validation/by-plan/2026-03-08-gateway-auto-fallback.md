---
plan: docs/plans/2026-03-08-gateway-auto-fallback.md
auditor: group-3-better-than-human-gateway-competitive-quality
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
UI-side feature: when C gateway is unreachable for 2.5s, ScApp swaps to
DemoGatewayClient and re-fires status events so all GatewayAwareLitElement
views populate. Plan frontmatter: `status: complete`.

## Key Claims (from the plan)
- `_fallbackTimer`, `_switchToDemo()`, `_inFallbackWindow` added to `ui/src/app.ts`
- Unit test that `DemoGatewayClient` reaches `connected` status
- E2E test that auto-fallback populates views without live gateway
- Disconnect banner suppressed during fallback window

## Evidence

### Implemented? (code exists) — FULL
- `ui/src/app.ts:546` — `@state() private _inFallbackWindow = false`
- `ui/src/app.ts:570` — `private _fallbackTimer: ReturnType<typeof setTimeout> | null`
- `ui/src/app.ts:977-988` — fallback timer setup + `_switchToDemo()`
- `ui/src/app.ts:949-950` — dynamic import of `DemoGatewayClient`
- `ui/src/app.ts:1121` — banner gated on `!this._inFallbackWindow`

### Proven? (tests exist) — FULL
- `ui/src/tests/gateway.test.ts:4` — `it("DemoGatewayClient reaches connected status within 500ms", ...)`
- `ui/e2e/app.spec.ts:187` — `test("auto-fallback populates chat without live gateway", ...)`

### Wired? (called in runtime path / dispatch) — FULL
- `connectedCallback()` arms `_fallbackTimer` (`ui/src/app.ts:977`).
- `_switchToDemo()` invoked from timer if gateway not connected
  (`ui/src/app.ts:986`).
- `disconnectedCallback()` clears the timer (`ui/src/app.ts:713-715`).
- `render()` branches on `_inFallbackWindow` (`ui/src/app.ts:1121`).

## Gaps
- E2E test wording shifted from plan ("populates overview") to actual
  ("populates chat") — semantic intent preserved.

## Notes
A tight UI-only plan, fully landed. No code-side dependencies.
