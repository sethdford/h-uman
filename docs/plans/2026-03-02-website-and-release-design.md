# SeaClaw Website & Full Release — Design Document

**Date:** 2026-03-02
**Status:** Approved

## Goal

Create a beautiful landing page + full documentation site using Astro + Starlight, deploy to GitHub Pages at `sethdford.github.io/seaclaw`, rename the GitHub repo from `nullclaw` to `seaclaw`, and update all docs so anyone can configure SeaClaw to work anywhere.

## Architecture

Static site built with Astro + Starlight. Landing page is a custom Astro page with hero, feature grid, benchmark table, and terminal demo. Docs section is Starlight-powered Markdown. GitHub Actions builds and deploys to `gh-pages` branch on push to main. The site lives in a `website/` directory in the repo.

## Tech Stack

- Astro 5 + Starlight (static site generator + docs theme)
- Tailwind CSS (landing page styling)
- GitHub Pages (hosting)
- GitHub Actions (CI/CD for deployment)

## Site Structure

```
/ Landing page
/docs/ Getting Started
/docs/installation/ Build from source, package managers
/docs/quickstart/ First run in 5 minutes
/docs/configuration/ Full config reference
/docs/providers/ Provider guides (50+ providers)
/docs/channels/ Channel setup (20 channels)
/docs/tools/ Tool reference (30+ tools)
/docs/memory/ Memory system guide
/docs/security/ Security model, sandboxing, pairing
/docs/peripherals/ Hardware (Arduino, STM32, RPi)
/docs/api/ Gateway API reference
/docs/contributing/ How to extend SeaClaw
```

## Landing Page Sections

1. **Hero**: Logo, tagline ("239 KB. < 5 MB RAM. Runs on anything."), CTA buttons (Get Started, GitHub)
2. **Stats bar**: Binary size, test count, provider count, channel count
3. **Features grid**: 6 cards (tiny binary, instant startup, fully portable, secure by default, fully swappable, local LLM support)
4. **Benchmark table**: Comparison with alternatives
5. **Architecture overview**: Vtable diagram showing swappable components
6. **Quick start**: Terminal-style code block showing build + first run
7. **Provider logos/grid**: Show the breadth of supported services
8. **Footer**: Links, license, GitHub

## Repo Changes

1. Rename GitHub repo `nullclaw` -> `seaclaw`
2. Update git remote URLs
3. Update all internal references to the repo URL
4. Merge `seaclaw/c-port-100-parity` branch to `main`
5. Update README stats (1,791 tests, 50+ providers)
6. Add `website/` directory with Astro project
7. Add GitHub Actions workflow for site deployment
8. Tag first release `v2026.3.2`
