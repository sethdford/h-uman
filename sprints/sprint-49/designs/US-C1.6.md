# Design for US-C1.6: Installation Guide & README Link

## Approach
Create a reader-segmented installation guide at `docs/guides/installation.md` that bridges the gap from "downloaded .pkg" or "found GitHub releases" to "daemon running and verified." The guide targets three personas: non-technical macOS users (quick-start path), developers (source build), and sysadmins (brew). Pair with a minimal README.md reorder that surfaces the install path above build instructions.

**Key insight:** Users want the shortest path to a working system. Gatekeeper warnings and permission dialogs are the only friction points after download; document them upfront so users aren't surprised.

**Versioning strategy:** Use GitHub Releases stable-URL pattern (`https://github.com/<org>/h-uman/releases/latest/download/human-macos-arm64.pkg`) to avoid re-documenting the version number on every release.

## Approach (continued)

1. **Three install paths**, each 3–5 paragraphs:
   - `.pkg` from GitHub Releases: explain download, double-click, gatekeeper prompt, permissions dialogs, success signal
   - Homebrew: minimal (brew install human, launchctl load), link to full pkg guide for permissions detail
   - From source: link to BUILDING.md for CMake instructions, minimal here

2. **Three permission/safety sections:**
   - Gatekeeper (why the warning, it's safe because notarized, link to Apple docs)
   - Full Disk Access (why needed: chat.db indexing, which System Settings tab, how to grant)
   - Accessibility (why needed: tapbacks and emoji reactions on iMessage, which System Settings tab)

3. **Verification path:** "Run `human doctor`" (forward-ref to US-C3; stub description here)

4. **Uninstall:** rm -rf /Applications/Human.app, plus `human uninstall` for daemon/config cleanup

5. **Troubleshooting:** top 5 issues (gatekeeper blocking, permissions denied, daemon won't start, config not found, port already in use) + link to `human doctor`

6. **Screenshot placeholders:** gatekeeper-prompt.png, full-disk-access.png, success-output.png (captured in separate follow-up, US-C5)

## Files to modify

| File | Change | Est. LOC |
|---|---|---|
| docs/guides/installation.md | Create new, 300 lines | +300 |
| README.md | Add "Install" section, reorder (move "Get Started" above "Build from Source") | +20 |
| tests/test_installation_guide.c | Verify markdown structure, check for broken links, validate required sections | +100 |
| .github/workflows/docs-lint.yml | New CI job: markdownlint + lychee link-check on docs/ | +30 |

## Implementation steps (for the implementer agent)

1. Create `docs/guides/installation.md` with skeleton headings (Quick start, Gatekeeper, Permissions, Verify, Uninstall, Homebrew, From source, Troubleshooting)
2. Fill "Quick start" section: download link + double-click, expect to see Gatekeeper, then app opens
3. Fill "Gatekeeper" section with explanation + Apple docs link
4. Fill "Permissions" sections (Full Disk Access, Accessibility) with System Settings paths
5. Fill "Verify" section with `human --version` + `human doctor` forward-ref note
6. Fill "Uninstall" section: rm /Applications/Human.app + human uninstall
7. Fill "Homebrew" and "From source" sections (brief, cross-link)
8. Fill "Troubleshooting" with 5 common issues
9. Add placeholder image references (docs/guides/img/) with notes on what to capture
10. Update README.md: add "Install" section above "Build", link to installation.md
11. Write tests in test_installation_guide.c: markdown parse, section presence, link validation
12. Add docs-lint.yml workflow: markdownlint + lychee
13. Run full test suite

## Risks

- **Screenshots become stale** (MEDIUM/SMALL): store separately in docs/guides/img/ with a note to update when UI changes. Mitigation: this story documents WHAT to capture; actual capture is US-C5 scope. Include a "last updated" comment in the markdown pointing to the release date.

- **macOS version assumptions** (LOW/SMALL): guide says "macOS 10.15+" (from US-C1.2 acceptance criteria). Mitigation: test validates the stated requirement matches CMakeLists.txt or docs/standards/.

- **Gatekeeper prompt varies by context** (LOW/SMALL): prompt text differs slightly between direct download and Finder. Mitigation: document the general shape ("You may see a prompt…") rather than exact wording; link to Apple's Gatekeeper docs for the source of truth.

- **`human doctor` command doesn't exist yet** (MEDIUM/MEDIUM): US-C3 implements it. Mitigation: this design uses a forward-ref ("This command lands in Sprint C3…") with a TODO. Test uses a stub function that checks the forward-ref comment exists.

- **Broken internal links in markdown** (LOW/SMALL): link to /Applications/Human.app or config paths. Mitigation: lychee CI job validates internal markdown links and HTTP links (with `--offline` for docs-only checking).

- **Permission paths vary by macOS version** (LOW/SMALL): System Settings UI changed in Big Sur and Monterey. Mitigation: document the "Privacy & Security" path (current stable); add a versioning comment if major paths shift.

## Test strategy

- **Unit (test_installation_guide.c):**
  - Markdown file exists and is ≥250 lines
  - Contains required section headings (8 sections enumerated)
  - Contains at least one reference to `human doctor` with forward-ref note
  - Contains at least one reference to gatekeeper with Apple docs link
  - Contains paths to Full Disk Access and Accessibility System Settings tabs
  - No unescaped backticks or unclosed code blocks (basic markdown linting)

- **Integration (docs-lint.yml):**
  - markdownlint on installation.md: validate MD001–MD051 rules (headers, spacing, lists)
  - lychee link-check: verify internal cross-links (to BUILDING.md, README.md) and external URLs (Apple docs) are reachable
  - CI blocks merge if lychee or lint fails

## Acceptance criteria mapping

- AC-C1.6.1 (three install paths) → Quick start (pkg), Homebrew (brew), From source (cmake link) sections
- AC-C1.6.2 (system requirements, what gets installed, verification, uninstall per path) → each section covers these; Verify section has `human doctor` stub
- AC-C1.6.3 (Gatekeeper section, why it's safe, Apple link) → dedicated "Gatekeeper" section with explanation + link
- AC-C1.6.4 (Troubleshooting, link to `human doctor`) → "Troubleshooting" section with top 5 issues, forward-ref to US-C3
- AC-C1.6.5 (README.md link) → README.md "Install" section added, links to installation.md
- AC-C1.6.6 (test: markdown exists, sections present, no broken links) → test_installation_guide.c covers all 6 checks

## Out of scope

- Actual notarization ticket or signed .pkg (US-C1.3)
- Screenshots (captured in US-C5)
- GUI installer or auto-update (non-goals per story)
- Detailed troubleshooting for every possible error (top 5 + link to `human doctor`)
- Private/internal-only docs (this is public user-facing guide)

## Version reference strategy

The guide will use the GitHub Releases stable URL pattern:
```
https://github.com/<org>/h-uman/releases/latest/download/human-macos-arm64.pkg
```

This URL always points to the latest release, so the markdown never needs versioning. The URL will be confirmed during implementation by checking the actual GitHub releases page structure.

## README.md changes

Current structure (provisional):
```
# human
<description>

## Quick Start
... build instructions ...

## Install
[NEW]
Quick intro, link to docs/guides/installation.md for full guide.
```

Proposed structure:
```
# human
<description>

## Install
Quick intro, link to docs/guides/installation.md.

## Quick Start (for developers)
... build instructions ...
```

(Exact diff provided during implementation after reading current README.)
