import { test, expect } from "@playwright/test";

const GOOGLE = /fonts\.(googleapis|gstatic)\.com/;

test("the Inter fallback is declared from the same origin", async ({ page }) => {
  await page.goto("/?demo");
  const srcs = await page.evaluate(() =>
    [...document.styleSheets].flatMap((s) => {
      try {
        return [...s.cssRules]
          .filter(
            (r) =>
              r instanceof CSSFontFaceRule &&
              r.style.getPropertyValue("font-family").includes("Inter"),
          )
          .map((r) => (r as CSSFontFaceRule).style.getPropertyValue("src"));
      } catch {
        return [];
      }
    }),
  );
  expect(srcs.length).toBeGreaterThan(0);
  for (const s of srcs) expect(s).not.toMatch(GOOGLE);
});

test("design-system view loads fonts only from its own origin", async ({ page, baseURL }) => {
  const origin = new URL(baseURL!).origin;
  const fontRequests: string[] = [];
  page.on("request", (r) => {
    if (r.resourceType() === "font" || GOOGLE.test(r.url())) fontRequests.push(r.url());
  });
  await page.goto("/?demo#design-system");
  const view = page.locator("hu-design-system-view");
  await view.waitFor();
  // Click the shadow-internal control, not the host's bounding-box center —
  // the host also contains the (much wider) "Quiet Room preview" label, so a
  // plain host click lands on inert label text instead of the switch.
  await view.locator('hu-switch[data-testid="quiet-toggle"]').locator(":scope .switch").click();
  await page.evaluate(() => document.fonts.ready);
  await page.waitForLoadState("networkidle");
  expect(
    fontRequests.some((u) => /newsreader/i.test(u)),
    `expected a Newsreader request, got: ${fontRequests.join(", ") || "none"}`,
  ).toBe(true);
  for (const u of fontRequests) expect(new URL(u).origin, u).toBe(origin);
});
