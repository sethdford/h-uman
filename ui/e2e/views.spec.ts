import { test, expect } from "@playwright/test";

test.describe("Secondary Views", () => {
  test("config view renders", async ({ page }) => {
    await page.goto("/#config");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const configView = app.locator("hu-config-view");
    await expect(configView).toBeAttached({ timeout: 5000 });
  });

  test("models view renders", async ({ page }) => {
    await page.goto("/#models");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const modelsView = app.locator("hu-models-view");
    await expect(modelsView).toBeAttached({ timeout: 5000 });
  });

  test("tools view renders", async ({ page }) => {
    await page.goto("/#tools");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const toolsView = app.locator("hu-tools-view");
    await expect(toolsView).toBeAttached({ timeout: 5000 });
  });

  test("sessions view renders", async ({ page }) => {
    await page.goto("/?demo#sessions");
    await page.waitForLoadState("domcontentloaded");
    const sessionsView = page.locator("hu-app >> hu-sessions-view");
    await expect(sessionsView).toBeAttached({ timeout: 5000 });
  });

  test("nodes view renders", async ({ page }) => {
    await page.goto("/#nodes");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const nodesView = app.locator("hu-nodes-view");
    await expect(nodesView).toBeAttached({ timeout: 5000 });
  });

  test("agents view renders", async ({ page }) => {
    await page.goto("/#agents");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const agentsView = app.locator("hu-agents-view");
    await expect(agentsView).toBeAttached({ timeout: 5000 });
  });

  test("automations view renders", async ({ page }) => {
    await page.goto("/#automations");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const automationsView = app.locator("hu-automations-view");
    await expect(automationsView).toBeAttached({ timeout: 5000 });
  });

  test("skills view renders", async ({ page }) => {
    await page.goto("/#skills");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const skillsView = app.locator("hu-skills-view");
    await expect(skillsView).toBeAttached({ timeout: 5000 });
  });

  test("voice view renders", async ({ page }) => {
    await page.goto("/#voice");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const voiceView = app.locator("hu-voice-view");
    await expect(voiceView).toBeAttached({ timeout: 5000 });
  });

  test("usage view renders", async ({ page }) => {
    await page.goto("/#usage");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const usageView = app.locator("hu-usage-view");
    await expect(usageView).toBeAttached({ timeout: 5000 });
  });

  test("security view renders", async ({ page }) => {
    await page.goto("/#security");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const securityView = app.locator("hu-security-view");
    await expect(securityView).toBeAttached({ timeout: 5000 });
  });

  test("logs view renders", async ({ page }) => {
    await page.goto("/#logs");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const logsView = app.locator("hu-logs-view");
    await expect(logsView).toBeAttached({ timeout: 5000 });
  });

  test("overview view renders", async ({ page }) => {
    await page.goto("/");
    await page.waitForLoadState("domcontentloaded");
    const app = page.locator("hu-app");
    await expect(app).toBeAttached({ timeout: 5000 });
    const overviewView = app.locator("hu-overview-view");
    await expect(overviewView).toBeAttached({ timeout: 5000 });
  });

  test("nodes view shows data table in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await expect(page.locator("hu-app >> hu-nodes-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-nodes-view");
        return !!view?.shadowRoot?.querySelector("hu-data-table-v2");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("nodes view shows page hero in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await expect(page.locator("hu-app >> hu-nodes-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-nodes-view");
        return !!view?.shadowRoot?.querySelector("hu-page-hero");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("nodes view shows refresh button in demo mode", async ({ page }) => {
    await page.goto("/?demo#nodes");
    await expect(page.locator("hu-app >> hu-nodes-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const text = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const view = app?.shadowRoot?.querySelector("hu-nodes-view");
        return view?.shadowRoot?.textContent ?? "";
      });
      expect(text).toContain("Refresh");
    }).toPass({ timeout: 8000 });
  });
});

test.describe("Skills View (Demo Mode)", () => {
  test("skills view shows stat cards in demo mode", async ({ page }) => {
    await page.goto("/?demo#skills");
    const view = page.locator("hu-app >> hu-skills-view");
    await expect(view).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return sv?.shadowRoot?.querySelectorAll("hu-stat-card").length ?? 0;
      });
      expect(count).toBe(4);
    }).toPass({ timeout: 8000 });
  });

  test("skills view renders installed skill cards", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return sv?.shadowRoot?.querySelectorAll("hu-skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThanOrEqual(1);
    }).toPass({ timeout: 8000 });
  });

  test("skills view installed cards have switches", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        const cards = sv?.shadowRoot?.querySelectorAll("hu-skill-card");
        let switches = 0;
        cards?.forEach((c) => {
          if (c.shadowRoot?.querySelector("hu-switch")) switches++;
        });
        return switches;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
  });

  test("skills view renders registry section", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return !!sv?.shadowRoot?.querySelector("hu-skill-registry");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view registry search input exists", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        const reg = sv?.shadowRoot?.querySelector("hu-skill-registry");
        return !!reg?.shadowRoot?.querySelector("hu-input");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view has toolbar with search", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return !!sv?.shadowRoot?.querySelector(".toolbar hu-input");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view detail sheet opens on card click", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const count = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return sv?.shadowRoot?.querySelectorAll("hu-skill-card").length ?? 0;
      });
      expect(count).toBeGreaterThan(0);
    }).toPass({ timeout: 8000 });
    await page.evaluate(() => {
      const app = document.querySelector("hu-app");
      const sv = app?.shadowRoot?.querySelector("hu-skills-view");
      const card = sv?.shadowRoot?.querySelector("hu-skill-card") as any;
      if (!card) return;
      card.dispatchEvent(
        new CustomEvent("skill-select", {
          detail: { skill: card.skill ?? { name: "test" } },
          bubbles: true,
          composed: true,
        }),
      );
    });
    await expect(async () => {
      const hasDetail = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return !!sv?.shadowRoot?.querySelector("hu-skill-detail");
      });
      expect(hasDetail).toBe(true);
    }).toPass({ timeout: 8000 });
  });

  test("skills view page hero renders", async ({ page }) => {
    await page.goto("/?demo#skills");
    await expect(page.locator("hu-app >> hu-skills-view")).toBeAttached({ timeout: 5000 });
    await expect(async () => {
      const exists = await page.evaluate(() => {
        const app = document.querySelector("hu-app");
        const sv = app?.shadowRoot?.querySelector("hu-skills-view");
        return !!sv?.shadowRoot?.querySelector("hu-page-hero");
      });
      expect(exists).toBe(true);
    }).toPass({ timeout: 8000 });
  });
});
