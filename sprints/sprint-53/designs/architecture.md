---
title: "Sprint 53 — Architectural design (cross-cutting)"
created: 2026-05-24
sprint: 53
status: design-only
---

# Sprint 53 cross-cutting design

Per-story acceptance criteria live in `../stories.md`. Sprint 53 is
docs-heavy so the "architecture" here is mostly about the doc-as-code
discipline.

## 1. Single source of truth: the markdown is the canon

Both `getting-started.md` (web view) and `human help getting-started`
(CLI view) render from the SAME markdown file at runtime. The CLI
parses the markdown live; it does NOT duplicate the content into a C
string array.

Why: copy-paste duplication is the #1 cause of stale docs. By
forcing the CLI to consume the markdown, a doc fix automatically
flows to every surface that displays it.

```c
/* include/human/cli/markdown_help.h */

typedef struct hu_markdown_section {
    char heading[128];
    char *body;          /* allocator-owned */
    size_t body_len;
} hu_markdown_section_t;

/* Parse markdown into sections (split by ## headings). Allocator-owned. */
hu_error_t hu_markdown_parse_sections(hu_allocator_t *alloc,
                                      const char *markdown,
                                      size_t len,
                                      hu_markdown_section_t **out,
                                      size_t *out_count);

/* Render one section to a terminal with ANSI escapes. */
hu_error_t hu_markdown_render_ansi(const hu_markdown_section_t *section,
                                   FILE *out);
```

The parser is intentionally minimal — split on `^## `, that's it. No
inline-formatting parsing beyond heading-bold + link-underline +
code-block-dim. Anything fancier requires a real markdown library
and that's out of scope.

## 2. Screenshot regeneration

```bash
# scripts/regen-onboarding-screenshots.sh
#
# Drives the wizard through each step against a fake-terminal
# capture (using `script` on macOS or `asciinema` if available),
# then runs `img2png` to produce deterministic PNGs.

for step in 1 2 3 4 5; do
  HU_IS_TEST=1 HU_FAKE_TERM_CAPTURE=img/getting-started/0${step}-*.png \
    ./build/human onboard --step ${step}
done
```

Pre-commit hook fires if `src/onboard/step_*.c` is staged AND the
screenshots haven't been regenerated. Stale screenshots are
catastrophic for getting-started UX — they show users a different
wizard than the one they'll actually see.

## 3. Length cap enforcement

```bash
# scripts/check-docs-length.sh
#
# Enforces hard caps on user-facing doc files. Anything in
# docs/guides/ that exceeds its cap fails the commit.

declare -A CAPS=(
  ["docs/guides/getting-started.md"]=400
  ["docs/guides/troubleshoot.md"]=500
  ["docs/guides/installation.md"]=600
  ["docs/guides/first-run.md"]=300
)

for path in "${!CAPS[@]}"; do
  if [ -f "$path" ]; then
    lines=$(wc -l < "$path")
    cap=${CAPS[$path]}
    if (( lines > cap )); then
      echo "FAIL: $path is $lines lines (cap: $cap)" >&2
      exit 1
    fi
  fi
done
```

User-facing docs that grow beyond their cap fail closed. Cap is
deliberate — once a getting-started doc is over 400 lines, it's a
reference manual, not a getting-started doc. Move content to a
linked-from page.

## 4. Doc CI gate composition

This sprint adds to the existing `scripts/doc-fleet.sh`:

```
doc-fleet.sh
├── (existing) check-standards.sh
├── (existing) check-terminology.sh
├── (existing) check-frontmatter.sh
├── (existing) check-md-links.sh             ← repo-wide markdown
├── (NEW)     check-docs-links.sh            ← guides-relative links
├── (NEW)     check-docs-code-blocks.sh      ← shellcheck bash blocks
└── (NEW)     check-docs-length.sh           ← hard line caps
```

All new checks fail closed on first violation, with a specific
error line naming the file + the violation.

## 5. README structure rule (US-C5.2)

```bash
# scripts/check-readme-structure.sh
#
# Asserts README.md has the expected first-h2 + four-bullet shape.

first_h2=$(grep -n '^## ' README.md | head -1)
if [[ "$first_h2" != *"Quick start"* ]]; then
  echo "FAIL: README.md first h2 must be '## Quick start' (got: $first_h2)" >&2
  exit 1
fi

# ... assert the 4 bullets present, getting-started link resolves
```

## 6. Test-fixture composition

| Fixture | Purpose | Story |
|---------|---------|-------|
| `tests/fixtures/markdown_sections.md` | Parser unit test input | US-C5.3 |
| `tests/fixtures/troubleshoot_doctor_fails.json` | Enumerated FAIL strings from doctor | US-C5.4 |
| `tests/fixtures/docs_broken_link.md` | Adversarial input for link-rot check | US-C5.5 |
| `tests/fixtures/docs_broken_codeblock.md` | Adversarial input for code-block check | US-C5.5 |

## 7. Build wire-up

```cmake
list(APPEND HU_CORE_SOURCES
    src/cli/markdown_help.c
    src/cli/help_getting_started.c
    src/cli/help_troubleshoot.c
)

list(APPEND HU_TEST_SOURCES
    tests/test_cli_help_getting_started.c
    tests/test_cli_help_troubleshoot.c
    tests/test_troubleshoot_doc_coverage.c
    tests/test_markdown_help_parser.c
)
```

The bash-script tests (US-C5.5) live under `tests/` but are not
C-compiled — they're invoked by the CI workflow + the pre-commit
hook directly.

## 8. Sizing recheck

| Story    | Source LoC | Markdown LoC | Test LoC | Total |
|----------|-----------|--------------|----------|-------|
| US-C5.1  | ~0        | ~400         | ~50      | 450   |
| US-C5.2  | ~50 (bash)| ~30 README   | ~80      | 160   |
| US-C5.3  | ~300      | ~0           | ~300     | 600   |
| US-C5.4  | ~30       | ~300         | ~200     | 530   |
| US-C5.5  | ~140 (bash)| ~0           | ~150     | 290   |
| **Total**| **~520**  | **~730**     | **~780** | **~2030** |

Backlog said "~800 LoC mostly markdown" — our recheck is higher (~2K)
because we're counting tests + CI scripts. The user-facing-docs total
(~730 markdown) matches the estimate spirit.

## 9. Where this design intentionally STOPS

- No Hugo / Jekyll / Docusaurus migration. Plain markdown on GitHub
  for now. A real docs site is its own sprint when the docs corpus
  warrants it.
- No video walkthroughs. Higher production cost than this sprint
  budget supports.
- No localization framework. English-only at ship time (matches
  Sprint 51 §US-C2 and Sprint 50 §C3 — defer i18n as a
  whole-product Sprint D topic).
- No "smart" doc generation (markdown from C comments). The two
  surfaces (web + CLI) read the SAME markdown, so we don't need
  generation — just one canonical file per topic.
- No "interactive" elements in the markdown beyond the standard
  GitHub rendering. CLI gets the press-Enter chunking; web view
  gets normal scrolling.
