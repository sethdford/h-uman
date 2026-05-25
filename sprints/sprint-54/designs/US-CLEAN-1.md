# Design for US-CLEAN-1: Normalize UNCERTAIN plan-dir frontmatter

## Approach

Mechanical doc edit. NOT a code change. NOT a re-audit. The prior audit
report (in `docs/plans/STATUS.md`) identified files needing frontmatter
normalization. The earlier triage report claimed 26 UNCERTAIN files; the
tech-lead's recon found ~20. Implementer must reconcile by running
**ground truth enumeration** first:

```bash
# Ground truth: every plan file under docs/plans/ that lacks
# an explicit canonical `status:` line in its frontmatter.
for f in docs/plans/*.md; do
  status=$(awk '/^---$/{n++} n==1 && /^status:/{print $2; exit}' "$f")
  case "$status" in
    closed|active|deferred|superseded|archived) ;;
    *) echo "$f" ;;
  esac
done | sort > /tmp/uncertain.txt
wc -l /tmp/uncertain.txt
```

Whatever count this returns (N) IS the work scope. Don't argue with the
audit's number; trust the empirical scan.

## Classification rules (per `docs/plans/STATUS.md` schema)

Implementer applies these rules in order, first match wins:

1. **Has `status: complete|shipped|implemented|done`** → normalize to `closed`
2. **Has `status: in-progress|wip|living|active`** → normalize to `active`
3. **Has `status: proposed|draft|scoped|not-started|design`** → normalize to `deferred`
4. **Has `status: obsolete|abandoned`** → normalize to `archived` AND leave file in place (US-CLEAN-1 does NOT move files; archival is a follow-up)
5. **No frontmatter at all** OR **frontmatter has no `status:` field** → use content heuristic:
   - Title or body mentions "phase ... DONE", "✅ shipped", "closed" → `closed`
   - Title or body mentions "deferred", "follow-up needed", "design done" → `deferred`
   - Modified in last 30 days AND has commits referencing it → `active`
   - Otherwise → `deferred` (safe default; can be re-classified later)
6. **`init-NN-*` design plans** (Sprint 11-design family) → all `deferred` per audit guidance

## Frontmatter shape (idempotent edit)

Each file's frontmatter must end up looking like:

```yaml
---
title: <preserved or extracted from H1>
status: <one of: closed|active|deferred|superseded|archived>
created: <preserved if present; else YYYY-MM-DD from filename>
last_audit: 2026-05-25
---
```

**Existing-frontmatter handling:**
- File has `---` block → REPLACE the `status:` line, ADD `last_audit:` if missing, preserve everything else
- File has NO `---` block → PREPEND a new block, then a blank line, then the original content

## Single commit

```
docs(plans): normalize N UNCERTAIN entries per STATUS.md schema

Per the 2026-05-25 audit in docs/plans/STATUS.md, normalize the
remaining files to canonical status frontmatter (closed | active |
deferred | superseded | archived). No body-text changes; only the
frontmatter block.

Verification: empty output from
  for f in docs/plans/*.md; do
    grep -Eq '^status: (closed|active|deferred|superseded|archived)$' \
      "$f" || echo "MISSING: $f"
  done
```

## Verification (must run before commit)

```bash
# (1) All plan files have canonical status
for f in docs/plans/*.md; do
  grep -Eq '^status: (closed|active|deferred|superseded|archived)$' \
    "$f" || echo "MISSING: $f"
done  # MUST be empty

# (2) No body content changed — git diff should show ONLY frontmatter
git diff --stat docs/plans/ | head
# Each file's "lines changed" should be small (~5-15)

# (3) Test suite unchanged — this is a docs-only PR
./build/human_tests --suite=imessage_ingest --suite=onboard_state | tail -3
# Results: ... unchanged from baseline
```

## Out of scope (defer to follow-up cleanup)

- Archival moves to `docs/plans/.archive/` (per STATUS.md, the 2
  archived-tagged files were already moved in the prior commit;
  re-archival of any new `archived`-tagged files happens in a separate
  PR)
- Updating `docs/plans/STATUS.md`'s audit table to match the normalized
  state (follow-up; this story focuses on the FILES, not the index)
- Re-classifying files whose existing `status:` value is canonical-but-
  wrong (e.g. a doc tagged `closed` that's clearly still in flight) —
  audit follow-up, not normalization

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Concurrent agent edits same plan file | Low | Medium | Wave 1 = first in sprint; sequential with no other plan stories |
| File has non-standard frontmatter delimiters (e.g. `+++` TOML) | Very low | Low | Implementer detects and skips; reports in commit message |
| 30-day modification heuristic catches files modified by `git checkout` rather than real edits | Medium | Low | Implementer cross-checks against `git log --follow` for actual commits |
| The agent's "20 not 26" finding masks a real audit error | Low | Low | Whatever the empirical count is, it's the truth; the audit was a hint, not the spec |

## Why this is the first wave

Smallest story by far. Lets the implementer pool warm up against a
mechanical task before tackling the harder Tier 1 code stories. If
the docs PR fails to land cleanly, that's a leading indicator that
the rest of the sprint has process problems — caught cheap.

RESULT_tech-lead=DESIGN_READY
