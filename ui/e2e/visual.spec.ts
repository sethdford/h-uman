import { test, expect } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";

const CROSS_PLATFORM_THRESHOLD = 0.12;

function snapshotExists(testInfo: { snapshotDir: string }, name: string): boolean {
  const snapshotPath = path.join(testInfo.snapshotDir, name);
  return fs.existsSync(snapshotPath);
}

test.describe("Visual Regression", () => {
  test("overview page screenshot", async ({ page }, testInfo) => {
    const snapName = `overview-${testInfo.project.name}-${process.platform}.png`;
    await page.goto("/");
    await page.waitForTimeout(1000);
    if (!snapshotExists(testInfo, snapName)) {
      test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
    }
    await expect(page).toHaveScreenshot("overview.png", {
      maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
    });
  });

  test("chat page screenshot", async ({ page }, testInfo) => {
    const snapName = `chat-${testInfo.project.name}-${process.platform}.png`;
    await page.goto("/#chat");
    await page.waitForTimeout(1000);
    if (!snapshotExists(testInfo, snapName)) {
      test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
    }
    await expect(page).toHaveScreenshot("chat.png", {
      maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
    });
  });

  test("catalog page screenshot", async ({ page }, testInfo) => {
    const snapName = `catalog-${testInfo.project.name}-${process.platform}.png`;
    await page.goto("/catalog.html");
    await page.waitForTimeout(1000);
    if (!snapshotExists(testInfo, snapName)) {
      test.skip(true, `No baseline for ${process.platform}, run --update-snapshots locally`);
    }
    await expect(page).toHaveScreenshot("catalog.png", {
      maxDiffPixelRatio: CROSS_PLATFORM_THRESHOLD,
    });
  });
});
