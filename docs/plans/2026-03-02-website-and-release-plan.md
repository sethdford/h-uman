# SeaClaw Website & Release Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a beautiful Astro + Starlight website with landing page and full docs, deploy to GitHub Pages, rename the repo, and ship a tagged release.

**Architecture:** Static site in `website/` directory using Astro 5 + Starlight for docs, custom landing page with Tailwind CSS. GitHub Actions deploys to gh-pages on push. All content authored in Markdown.

**Tech Stack:** Astro 5, Starlight, Tailwind CSS, GitHub Actions, GitHub Pages

---

## Phase 1: Scaffold Website

### Task 1: Initialize Astro + Starlight project

**Files:**

- Create: `website/package.json`
- Create: `website/astro.config.mjs`
- Create: `website/tsconfig.json`
- Create: `website/src/content/docs/index.mdx` (docs landing)

**Step 1:** Scaffold Astro project with Starlight

```bash
cd /Users/sethford/Documents/nullclaw
npm create astro@latest website -- --template starlight --no-git --no-install
cd website && npm install
npm install @astrojs/tailwind tailwindcss
```

**Step 2:** Configure astro.config.mjs with Starlight + Tailwind, set base path for GitHub Pages (`/seaclaw/`), configure sidebar nav structure, site title, social links, logo.

**Step 3:** Verify dev server starts

```bash
cd website && npm run dev
```

Expected: Site loads at localhost:4321

**Step 4:** Commit

```bash
git add website/
git commit -m "feat: scaffold Astro + Starlight website"
```

---

## Phase 2: Landing Page

### Task 2: Create custom landing page

**Files:**

- Create: `website/src/pages/index.astro`
- Create: `website/src/components/Hero.astro`
- Create: `website/src/components/Stats.astro`
- Create: `website/src/components/Features.astro`
- Create: `website/src/components/Benchmark.astro`
- Create: `website/src/components/QuickStart.astro`
- Create: `website/src/components/Footer.astro`
- Copy: `seaclaw.svg` -> `website/public/seaclaw.svg`

**Step 1:** Create the landing page layout (`src/pages/index.astro`) with dark theme, the SeaClaw color palette (deep ocean blues/teals + red claw accent from the logo).

**Step 2:** Build Hero component — large logo, tagline "239 KB binary. < 5 MB RAM. Boots in 2ms. Runs on anything with a CPU.", two CTA buttons: "Get Started" (-> /docs/) and "GitHub" (-> repo).

**Step 3:** Build Stats bar — four stat cards in a row: "239 KB" binary, "1,791" tests, "50+" providers, "20" channels.

**Step 4:** Build Features grid — 6 cards:

- Impossibly Small (239 KB static binary)
- Instant Startup (<2ms on Apple Silicon)
- True Portability (ARM, x86, RISC-V)
- Secure by Default (sandbox, pairing, encryption)
- Fully Swappable (vtable architecture)
- Local LLM Ready (llama.cpp, Ollama, vLLM)

**Step 5:** Build Benchmark table — the comparison table from README, styled as a responsive component.

**Step 6:** Build QuickStart — terminal-style code block showing git clone, build, and first run.

**Step 7:** Build Footer with links to docs, GitHub, license.

**Step 8:** Verify landing page renders correctly

```bash
cd website && npm run dev
# Open http://localhost:4321
```

**Step 9:** Commit

```bash
git add website/src/ website/public/
git commit -m "feat: add landing page with hero, features, benchmarks"
```

---

## Phase 3: Documentation Content

### Task 3: Write core docs pages

**Files:**

- Create: `website/src/content/docs/getting-started/index.mdx`
- Create: `website/src/content/docs/getting-started/installation.mdx`
- Create: `website/src/content/docs/getting-started/quickstart.mdx`
- Create: `website/src/content/docs/getting-started/configuration.mdx`

Content: Installation from source (macOS, Linux, Windows/WSL), prerequisites, cmake commands, onboard wizard, config.json reference, environment variable overrides.

### Task 4: Write provider docs

**Files:**

- Create: `website/src/content/docs/providers/index.mdx` (overview + provider table)
- Create: `website/src/content/docs/providers/cloud.mdx` (OpenAI, Anthropic, Gemini, OpenRouter, etc.)
- Create: `website/src/content/docs/providers/local.mdx` (llama.cpp, Ollama, LM Studio, vLLM, sglang)
- Create: `website/src/content/docs/providers/compatible.mdx` (any OpenAI-compatible API)

Content: For each provider — config snippet, env vars, model selection, troubleshooting.

### Task 5: Write channel docs

**Files:**

- Create: `website/src/content/docs/channels/index.mdx` (overview)
- Create: `website/src/content/docs/channels/cli.mdx`
- Create: `website/src/content/docs/channels/telegram.mdx`
- Create: `website/src/content/docs/channels/discord.mdx`
- Create: `website/src/content/docs/channels/slack.mdx`
- Create: `website/src/content/docs/channels/signal.mdx`
- Create: `website/src/content/docs/channels/nostr.mdx`
- Create: `website/src/content/docs/channels/more.mdx` (IRC, Matrix, WhatsApp, Line, etc.)

Content: For each channel — setup steps, config, allowlists, troubleshooting.

### Task 6: Write remaining docs

**Files:**

- Create: `website/src/content/docs/tools/index.mdx`
- Create: `website/src/content/docs/memory/index.mdx`
- Create: `website/src/content/docs/security/index.mdx`
- Create: `website/src/content/docs/peripherals/index.mdx`
- Create: `website/src/content/docs/api/index.mdx`
- Create: `website/src/content/docs/contributing/index.mdx`

Content derived from README sections + source code.

### Task 7: Commit docs

```bash
git add website/src/content/
git commit -m "docs: add complete documentation site content"
```

---

## Phase 4: Deploy Pipeline

### Task 8: GitHub Actions deployment workflow

**Files:**

- Create: `.github/workflows/deploy-website.yml`

Workflow: On push to main (when website/ changes), install Node, build Astro, deploy to gh-pages branch using `peaceiris/actions-gh-pages`.

**Step 1:** Write the workflow YAML.

**Step 2:** Commit

```bash
git add .github/workflows/deploy-website.yml
git commit -m "ci: add GitHub Pages deployment for website"
```

---

## Phase 5: Repo Rename & Release

### Task 9: Rename GitHub repo

**Step 1:** Rename repo via GitHub API

```bash
gh repo rename seaclaw
```

**Step 2:** Update local remote

```bash
git remote set-url origin https://github.com/sethdford/seaclaw
```

**Step 3:** Update README URLs, badge links, clone URLs to use new repo name.

### Task 10: Update README stats

Update test count (1,791), provider count (50+), and any other stale numbers.

### Task 11: Merge to main and tag release

**Step 1:** Merge branch to main

```bash
git checkout main
git merge seaclaw/c-port-100-parity
```

**Step 2:** Tag release

```bash
git tag -a v2026.3.2 -m "SeaClaw v2026.3.2 — First public release"
git push origin main --tags
```

**Step 3:** Create GitHub release

```bash
gh release create v2026.3.2 --title "SeaClaw v2026.3.2" --notes "First public release..."
```
