import { test, expect } from "@playwright/test";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const QUIET = JSON.parse(
  readFileSync(
    fileURLToPath(new URL("../../design-tokens/quiet.tokens.json", import.meta.url)),
    "utf-8",
  ),
).quiet;
// getComputedStyle returns custom-property values verbatim — the browser does
// not reserialize them. The mismatch is upstream: Lightning CSS's production
// minification pass strips insignificant trailing AND leading zeros
// (0.010 -> .01), while quiet.tokens.json's literal keeps the spec table's
// "0.010" spelling. Canonicalize decimal numbers the same way (see the
// similar note in quiet-cascade.spec.ts, which doesn't need the leading-zero
// case because it loads the unminified source CSS) so the two sources agree
// regardless of formatting.
const norm = (s: string) =>
  s
    .trim()
    .replace(/\s+/g, " ")
    .replace(/-?\d*\.\d+/g, (n) => String(parseFloat(n)));

test("the BUILT bundle resolves quiet tokens after the toggle", async ({ page }) => {
  await page.emulateMedia({ colorScheme: "light" });
  await page.goto("/?demo#design-system");
  const view = page.locator("hu-design-system-view");
  await view.waitFor();
  const bg = () =>
    page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--hu-bg"));
  expect(norm(await bg())).not.toBe(norm(QUIET.light.bg.$value));
  // Click the shadow-internal control, not the host's bounding-box center —
  // the host also contains the (much wider) "Quiet Room preview" label, so a
  // plain host click lands on inert label text instead of the switch.
  await view.locator('hu-switch[data-testid="quiet-toggle"]').locator(":scope .switch").click();
  await expect.poll(async () => norm(await bg())).toBe(norm(QUIET.light.bg.$value));
  const family = await view
    .locator(".quiet-display")
    .evaluate((el) => getComputedStyle(el).fontFamily);
  expect(family).toContain("Newsreader Variable");
});
