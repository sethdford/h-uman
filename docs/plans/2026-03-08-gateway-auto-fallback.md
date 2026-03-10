# Gateway Auto-Fallback Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** When the C gateway is unavailable, automatically fall back to DemoGatewayClient so all 16 views populate with data instead of showing empty skeletons.

**Architecture:** Add a fallback timer in `ScApp.connectedCallback()`. The real `GatewayClient` gets 2.5 seconds to reach `"connected"` status. If it doesn't, we swap to `DemoGatewayClient`, update the global gateway provider, and re-fire status events so all `GatewayAwareLitElement` views load their data. A subtle toast indicates demo mode.

**Tech Stack:** LitElement, TypeScript, Vitest, Playwright

---

### Task 1: Add unit test for auto-fallback behavior

**Files:**

- Modify: `ui/src/tests/gateway.test.ts`

**Step 1: Write the failing test**

Add a test that verifies the fallback logic we'll extract. Since `ScApp` is a full web component, we'll test the fallback at the integration level in task 3 (e2e). For unit tests, add a test verifying `DemoGatewayClient` connects and fires status events correctly (validates our assumption that the fallback will work):

```typescript
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
```

**Step 2: Run test to verify it passes**

Run: `cd ui && npx vitest run src/tests/gateway.test.ts`
Expected: PASS (this validates demo gateway works as expected)

**Step 3: Commit**

```
test(ui): verify DemoGatewayClient connects and fires status events
```

---

### Task 2: Implement auto-fallback in app.ts

**Files:**

- Modify: `ui/src/app.ts`

**Step 1: Add fallback timer logic to `connectedCallback()`**

In `ScApp`, after `this.gateway.connect(wsUrl)`, add a fallback timer. If the gateway doesn't reach `"connected"` within 2500ms, swap to `DemoGatewayClient`.

Replace the end of `connectedCallback()` (after `this.gateway.connect(wsUrl);`) with:

```typescript
// Auto-fallback: if real gateway doesn't connect within 2.5s, use demo
if (!this._isDemo) {
  this._fallbackTimer = setTimeout(() => {
    if (this.gateway?.status !== "connected") {
      this._switchToDemo();
    }
  }, 2500);
  // Cancel fallback if real gateway connects in time
  this.gateway.addEventListener("status", ((e: CustomEvent<string>) => {
    if (e.detail === "connected" && this._fallbackTimer) {
      clearTimeout(this._fallbackTimer);
      this._fallbackTimer = null;
    }
  }) as EventListener);
}
```

Add the `_fallbackTimer` property and `_switchToDemo()` method:

```typescript
private _fallbackTimer: ReturnType<typeof setTimeout> | null = null;

private _switchToDemo(): void {
  this._fallbackTimer = null;
  // Tear down real gateway
  this.gateway?.removeEventListener("status", this._statusHandler);
  this.gateway?.disconnect();
  // Create and wire demo gateway
  const demo = new DemoGatewayClient() as unknown as GatewayClient;
  this.gateway = demo;
  setGateway(demo);
  demo.addEventListener("status", this._statusHandler);
  demo.connect("demo://fallback");
}
```

Also clean up the timer in `disconnectedCallback()`:

```typescript
if (this._fallbackTimer) {
  clearTimeout(this._fallbackTimer);
  this._fallbackTimer = null;
}
```

**Step 2: Run typecheck**

Run: `cd ui && npm run typecheck`
Expected: 0 errors

**Step 3: Run unit tests**

Run: `cd ui && npm run test`
Expected: All tests pass

**Step 4: Manual smoke test**

Run: `cd ui && npm run dev`
Open `http://localhost:5173/` (no `?demo`). Views should populate within ~3 seconds.

**Step 5: Commit**

```
feat(ui): auto-fallback to demo gateway when server unavailable
```

---

### Task 3: Add e2e test for auto-fallback

**Files:**

- Modify: `ui/e2e/app.spec.ts`

**Step 1: Write the e2e test**

Add a test that verifies the overview view populates with data when no gateway is running (relies on auto-fallback):

```typescript
test("auto-fallback populates overview without live gateway", async ({
  page,
}) => {
  await page.goto("/");
  // Wait for fallback timer (2.5s) + demo gateway connect (400ms) + view load
  const overview = page.locator("hu-app >> hu-overview-view");
  await expect(overview).toBeAttached({ timeout: 5000 });
  // Verify actual content loaded (not just skeleton)
  await expect(async () => {
    const text = await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const view = app?.shadowRoot?.querySelector("hu-overview-view");
      return view?.shadowRoot?.textContent ?? "";
    });
    // Overview should show capabilities data from demo gateway
    expect(text.length).toBeGreaterThan(100);
  }).toPass({ timeout: 8000 });
});
```

**Step 2: Run the e2e test**

Run: `cd ui && npx playwright test e2e/app.spec.ts --grep "auto-fallback"`
Expected: PASS

**Step 3: Commit**

```
test(ui): e2e test for gateway auto-fallback
```

---

### Task 4: Clean up disconnected banner for demo mode

**Files:**

- Modify: `ui/src/app.ts`

**Step 1: Suppress disconnect banner during fallback window**

Currently the disconnect banner shows immediately when status is `"disconnected"`. During the 2.5s fallback window this flashes briefly. Add a flag to suppress it during the fallback period.

Add property:

```typescript
@state() private _inFallbackWindow = false;
```

Set `_inFallbackWindow = true` before `this.gateway.connect(wsUrl)` (only when `!this._isDemo`), and set it to `false` inside `_switchToDemo()` and in the "connected" status handler that clears the timer.

Update the disconnect banner condition in `render()`:

```typescript
${this.connectionStatus === "disconnected" && !this._inFallbackWindow
  ? html`<div class="disconnect-banner" role="alert">...</div>`
  : nothing}
```

**Step 2: Run typecheck + tests**

Run: `cd ui && npm run typecheck && npm run test`
Expected: PASS

**Step 3: Commit**

```
fix(ui): suppress disconnect banner during fallback window
```

---

### Task 5: Run full check suite

**Step 1: Run all checks**

Run: `cd ui && npm run check`
Expected: typecheck, lint, format, test, lint:tokens all pass

**Step 2: Run e2e tests**

Run: `cd ui && npx playwright test`
Expected: All tests pass (both `?demo` tests and auto-fallback tests)

**Step 3: Final commit if any fixups needed**

---

## Summary of Changes

| File                           | Change                                                            |
| ------------------------------ | ----------------------------------------------------------------- |
| `ui/src/app.ts`                | Add `_fallbackTimer`, `_switchToDemo()`, `_inFallbackWindow` flag |
| `ui/src/tests/gateway.test.ts` | Add DemoGatewayClient connection test                             |
| `ui/e2e/app.spec.ts`           | Add auto-fallback e2e test                                        |

**Total: ~40 lines of new code across 3 files.** No changes to demo-gateway.ts, gateway.ts, gateway-aware.ts, or any view files.
