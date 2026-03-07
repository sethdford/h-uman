import { test, expect } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";

const CROSS_PLATFORM_THRESHOLD = 0.12;

function snapshotExists(testInfo: { snapshotDir: string }, name: string): boolean {
  const snapshotPath = path.join(testInfo.snapshotDir, name);
  return fs.existsSync(snapshotPath);
}

const ALL_VIEWS = [
  { name: "overview", hash: "" },
  { name: "chat", hash: "#chat" },
  { name: "agents", hash: "#agents" },
  { name: "models", hash: "#models" },
  { name: "config", hash: "#config" },
  { name: "tools", hash: "#tools" },
  { name: "channels", hash: "#channels" },
  { name: "automations", hash: "#automations" },
  { name: "skills", hash: "#skills" },
  { name: "voice", hash: "#voice" },
  { name: "nodes", hash: "#nodes" },
  { name: "usage", hash: "#usage" },
  { name: "security", hash: "#security" },
  { name: "logs", hash: "#logs" },
];

test.describe("Visual Regression — Dark Theme", () => {
  for (const view of ALL_VIEWS) {
    test(`${view.name} (dark)`, async ({ page }, testInfo) => {
      const snapName = `${view.name}-dark-${testInfo.project.name}-${process.platform}.png`;
      const url = view.hash ? `/?demo${view.hash}` : "/?demo";
      await page.goto(url);
      await page.waitForTimeout(1500);
      const updating = testInfo.config.updateSnapshots !== "none";
      if (!updating && !snapshotExists(testInfo, snapName)) {
        test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
      }
      await expect(page).toHaveScreenshot(`${view.name}-dark.png`, {
        maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
      });
    });
  }
});

test.describe("Visual Regression — Light Theme", () => {
  for (const view of ALL_VIEWS) {
    test(`${view.name} (light)`, async ({ page }, testInfo) => {
      const snapName = `${view.name}-light-${testInfo.project.name}-${process.platform}.png`;
      const url = view.hash ? `/?demo${view.hash}` : "/?demo";
      await page.goto(url);
      await page.waitForTimeout(1000);
      await page.evaluate(() => {
        document.documentElement.setAttribute("data-theme", "light");
      });
      await page.waitForTimeout(500);
      const updating = testInfo.config.updateSnapshots !== "none";
      if (!updating && !snapshotExists(testInfo, snapName)) {
        test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
      }
      await expect(page).toHaveScreenshot(`${view.name}-light.png`, {
        maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
      });
    });
  }
});

test.describe("Visual Regression — Catalog", () => {
  test("catalog page screenshot", async ({ page }, testInfo) => {
    const snapName = `catalog-${testInfo.project.name}-${process.platform}.png`;
    await page.goto("/catalog.html");
    await page.waitForTimeout(1000);
    const updating = testInfo.config.updateSnapshots !== "none";
    if (!updating && !snapshotExists(testInfo, snapName)) {
      test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
    }
    await expect(page).toHaveScreenshot("catalog.png", {
      maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
    });
  });
});
