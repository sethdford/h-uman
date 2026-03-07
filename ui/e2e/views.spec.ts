import { test, expect } from "@playwright/test";

test.describe("Secondary Views", () => {
  test("config view renders", async ({ page }) => {
    await page.goto("/#config");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const configView = app.locator("sc-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("models view renders", async ({ page }) => {
    await page.goto("/#models");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const modelsView = app.locator("sc-models-view");
    await expect(modelsView).toBeAttached({ timeout: 5000 });
  });

  test("tools view renders", async ({ page }) => {
    await page.goto("/#tools");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const toolsView = app.locator("sc-tools-view");
    await expect(toolsView).toBeAttached({ timeout: 5000 });
  });

  test("sessions view redirects to chat", async ({ page }) => {
    await page.goto("/?demo#sessions");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        return !!app?.shadowRoot?.querySelector("sc-chat-view");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 10000 });
  });

  test("nodes view renders", async ({ page }) => {
    await page.goto("/#nodes");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const nodesView = app.locator("sc-nodes-view");
    await expect(nodesView).toBeAttached({ timeout: 5000 });
  });

  test("agents view renders", async ({ page }) => {
    await page.goto("/#agents");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const agentsView = app.locator("sc-agents-view");
    await expect(agentsView).toBeAttached({ timeout: 5000 });
  });

  test("automations view renders", async ({ page }) => {
    await page.goto("/#automations");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const automationsView = app.locator("sc-automations-view");
    await expect(automationsView).toBeAttached({ timeout: 5000 });
  });

  test("skills view renders", async ({ page }) => {
    await page.goto("/#skills");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const skillsView = app.locator("sc-skills-view");
    await expect(skillsView).toBeAttached({ timeout: 5000 });
  });

  test("voice view renders", async ({ page }) => {
    await page.goto("/#voice");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const voiceView = app.locator("sc-voice-view");
    await expect(voiceView).toBeAttached({ timeout: 5000 });
  });

  test("usage view renders", async ({ page }) => {
    await page.goto("/#usage");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const usageView = app.locator("sc-usage-view");
    await expect(usageView).toBeAttached({ timeout: 5000 });
  });

  test("security view renders", async ({ page }) => {
    await page.goto("/#security");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const securityView = app.locator("sc-security-view");
    await expect(securityView).toBeAttached({ timeout: 5000 });
  });

  test("logs view renders", async ({ page }) => {
    await page.goto("/#logs");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const logsView = app.locator("sc-logs-view");
    await expect(logsView).toBeAttached({ timeout: 5000 });
  });

  test("overview view renders", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(500);
    const app = page.locator("sc-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const overviewView = app.locator("sc-overview-view");
    await expect(overviewView).toBeAttached({ timeout: 5000 });
  });

  test("nodes view shows data table in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-nodes-view");
        return !!view?.shadowRoot?.querySelector("sc-data-table-v2");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("nodes view shows page hero in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-nodes-view");
        return !!view?.shadowRoot?.querySelector("sc-page-hero");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("nodes view shows refresh button in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const text = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const view = app?.shadowRoot?.querySelector("sc-nodes-view");
        return view?.shadowRoot?.textContent ?? "";
      });
      expect(text).toContain("Refresh");
    }).toPass({ timeout: 8000 });
  });
});

test.describe("Skills View (Demo Mode)", () => {
  test("skills view shows stat cards in demo mode", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    const view = page.locator("sc-app >> sc-skills-view");
    await expect(view).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return sv?.shadowRoot?.querySelectorAll("sc-stat-card").length ?? 0;
      });
      expect(count).toBe(4);
    }).toPass({ timeout: 8000 });
  });

  test("skills view renders installed skill cards", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        const sections = sv?.shadowRoot?.querySelectorAll(".section");
        return sections?.[0]?.querySelectorAll(".skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: 8000 });
  });

  test("skills view installed cards have switches", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return sv?.shadowRoot?.querySelectorAll(".skill-card sc-switch").length ?? 0;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
  });

  test("skills view renders registry section", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        const sections = sv?.shadowRoot?.querySelectorAll(".section");
        return sections?.length ?? 0;
      });
      expect(count).toBeGreaterThanOrEqual(2);
    }).toPass({ timeout: 8000 });
  });

  test("skills view registry search input exists", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return !!sv?.shadowRoot?.querySelector(".registry-search-row sc-input");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view has toolbar with search", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return !!sv?.shadowRoot?.querySelector(".toolbar sc-input");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view detail sheet opens on card click", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return sv?.shadowRoot?.querySelectorAll(".skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      const card = sv?.shadowRoot?.querySelector(".skill-card") as HTMLElement;
      card?.click();
    });
    await page.waitForTimeout(500);
    const hasSheet = await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      return !!sv?.shadowRoot?.querySelector("sc-sheet");
    });
    expect(hasSheet).toBe(true);
  });

  test("skills view detail sheet shows detail name", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return sv?.shadowRoot?.querySelectorAll(".skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      const card = sv?.shadowRoot?.querySelector(".skill-card") as HTMLElement;
      card?.click();
    });
    await page.waitForTimeout(500);
    const hasName = await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      return !!sv?.shadowRoot?.querySelector(".detail-name");
    });
    expect(hasName).toBe(true);
  });

  test("skills view detail sheet has action buttons", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return sv?.shadowRoot?.querySelectorAll(".skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
    await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      const card = sv?.shadowRoot?.querySelector(".skill-card") as HTMLElement;
      card?.click();
    });
    await page.waitForTimeout(500);
    const btnCount = await page.evaluate(() => {
      const app = document.querySelector("sc-app");
      const sv = app?.shadowRoot?.querySelector("sc-skills-view");
      return sv?.shadowRoot?.querySelectorAll(".detail-actions sc-button").length ?? 0;
    });
    expect(btnCount).toBeGreaterThanOrEqual(2);
  });

  test("skills view page hero renders", async ({ page }) => {
    await page.goto("/?demo#skills");
    await page.waitForTimeout(1500);
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("sc-app");
        const sv = app?.shadowRoot?.querySelector("sc-skills-view");
        return !!sv?.shadowRoot?.querySelector("sc-page-hero");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });
});
