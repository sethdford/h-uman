import { test, expect } from "@playwright/test";

const GOOGLE = /fonts\.(googleapis|gstatic)\.com/;

// A second test ("design-system view loads fonts only from its own origin")
// is appended in Task 6, once the specimen card actually uses Newsreader.

test("the Inter fallback is declared from the same origin", async ({ page }) => {
  await page.goto("/?demo");
  const srcs = await page.evaluate(() =>
    [...document.styleSheets].flatMap((s) => {
      try {
        return [...s.cssRules]
          .filter((r) => r instanceof CSSFontFaceRule && r.style.getPropertyValue("font-family").includes("Inter"))
          .map((r) => (r as CSSFontFaceRule).style.getPropertyValue("src"));
      } catch {
        return [];
      }
    }),
  );
  expect(srcs.length).toBeGreaterThan(0);
  for (const s of srcs) expect(s).not.toMatch(GOOGLE);
});
