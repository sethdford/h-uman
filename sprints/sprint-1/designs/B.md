---
title: "Design — Story B: Orchestrator writes canonical A/B status path"
sprint: 1
story: B
created: 2026-05-11
status: ready_for_implementation
authored_by: tech-lead
---

# Design for Story B — Orchestrator writes canonical A/B status path

## Approach

The current `scripts/lora-runner-ab.sh` already produces the exact JSON
artifact the gateway handler wants to read — `$STATUS_JSON` at line 188
(`"$HUMAN_BIN" ml fidelity-status … --output "$STATUS_JSON"`). The C
handler `cp_admin_metrics_fidelity` (`src/gateway/cp_admin.c:1119–1155`)
resolves the canonical path with two rules: `$HUMAN_FIDELITY_AB_PATH` if
set and non-empty, otherwise `${HOME}/.human/last_fidelity_ab.json`. The
two surfaces simply don't talk yet.

The smallest change that closes the loop is a single trailing
**publish-after-success** block in `lora-runner-ab.sh`: after
`fidelity-status` returns 0 (i.e. the `set -e` guard hasn't already
aborted), copy `$STATUS_JSON` to a tmp file *next to the destination*,
`mv` it into place, and we're done. No new dependencies, no schema
change, no caller change. The atomicity guarantee comes for free from
`rename(2)` semantics on POSIX (man 2 rename: "If `newpath` already
exists, it will be atomically replaced") provided source and destination
sit on the same filesystem — which we ensure by deriving the tmp path
from `$dest`, not from `$TMPDIR`.

A `--no-publish` flag is added so test harnesses (and the future Story D
descope path) can exercise the orchestrator end-to-end without touching
the user's `~/.human/`. We deliberately default `--no-publish` to
**false** — the publish is the whole point of the story, and a missing
flag should not silently disable it.

Two design choices worth noting:

1. **Publish AFTER `fidelity-status`, not after the `lora-ab`
   comparator.** `lora-ab` exits with the gate result; `fidelity-status`
   produces the JSON the dashboard actually reads. If we published
   between the two, a comparator-gate failure (exit 3) would still
   ship a status to the dashboard — but the dashboard's "delta" tile
   would then disagree with CI's "this run failed the gate". Publishing
   strictly after `fidelity-status` keeps "what the user sees" and
   "what CI accepted" in lockstep.

2. **Put the publish block inside the existing `set -e`/`trap cleanup
   EXIT` regime, not a new function.** The script is 191 lines; a
   `publish_status()` helper would be 8 lines, half of which would be
   variable plumbing. Inlining the block at the call site keeps the
   linear-script readability the rest of the file already has.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `scripts/lora-runner-ab.sh` | add `NO_PUBLISH` flag + `--no-publish` argparse case + 6-line publish block at end | +12 |

That's the entire patch. No C, no UI, no test fixture, no CMake. The
test plan below uses ad-hoc shell invocations against the existing
script (no new test files required for AC-B.1 through AC-B.6).

## Patch shape (minimal-impact diff outline)

Three insertion points in `scripts/lora-runner-ab.sh` (line numbers
refer to current `HEAD`):

### 1. Flag default (after line 81, with the other flag defaults)

```bash
KEEP=0
NO_PUBLISH=0
```

### 2. Argparse case (inside the `while [[ $# -gt 0 ]]` loop, alongside `--keep` at line 93)

```bash
        --keep) KEEP=1; shift ;;
        --no-publish) NO_PUBLISH=1; shift ;;
```

### 3. Publish block (immediately after the `[lora-runner-ab] PASS` echo at line 189, before the artifacts echo at line 190)

```bash
# Publish status.json to the canonical path that
# `cp_admin_metrics_fidelity` reads. Atomic: write to a tmp file on
# the same filesystem as the destination, then rename. `set -e`
# already aborted on every prior failure path (exit 1=bad args,
# exit 2=empty response set, exit 3=gate fail), so reaching this
# line means the run produced a valid status.json.
if [[ "$NO_PUBLISH" -eq 0 ]]; then
    dest="${HUMAN_FIDELITY_AB_PATH:-${HOME:?HOME unset; pass --no-publish or set HUMAN_FIDELITY_AB_PATH}/.human/last_fidelity_ab.json}"
    mkdir -p "$(dirname "$dest")"
    tmpfile="$(mktemp "${dest}.XXXXXX")"
    cp "$STATUS_JSON" "$tmpfile"
    mv "$tmpfile" "$dest"
    echo "[lora-runner-ab] published → $dest"
fi
```

### Walk of success vs. failure code paths

| Step in script | Exit on failure | Publish reached? |
|---|---|---|
| Argparse (lines 83–101) | 1 | no — `set -e` + explicit `exit 1` |
| `mktemp` / `mkdir -p $OUTPUT_DIR` (lines 119–123) | 1 | no — `set -e` |
| Step 1 BASE responses (line 159) | non-zero from `human ml lora-runner` | no — `set -e` |
| Step 1 empty-response check (lines 160–163) | 2 | no — explicit `exit 2` |
| Step 2 ADAPTER responses (line 166) | non-zero from `human ml lora-runner` | no — `set -e` |
| Step 2 empty-response check (lines 168–171) | 2 | no — explicit `exit 2` |
| Step 3 comparator (lines 178–181) | 3 | no — explicit `exit 3` |
| Step 3 `fidelity-status` (lines 186–188) | non-zero from `human ml fidelity-status` | no — `set -e` |
| **PASS branch** (line 189 onward) | 0 | **YES** — only this path |

Net result: the publish block is reachable iff every prior step
returned 0. AC-B.5's exit-2 (no-provider) path triggers at lines 161
or 168, well before the publish block.

## Canonical-path resolution

The destination is selected with the same precedence the C handler
uses, so a test harness can override either side by setting one
environment variable.

| Precedence | Selector | Source-of-truth on the C side |
|---|---|---|
| 1 | `$HUMAN_FIDELITY_AB_PATH` (if set & non-empty) | `cp_admin.c:1142` |
| 2 | `${HOME}/.human/last_fidelity_ab.json` | `cp_admin.c:1153` |

The shell expression `${HUMAN_FIDELITY_AB_PATH:-${HOME:?…}/.human/last_fidelity_ab.json}`
implements both rules:

- `${HUMAN_FIDELITY_AB_PATH:-…}` — use the env var if set and non-empty,
  otherwise fall through to the default. This matches the C-side
  guard `if (override && override[0])` exactly: an exported empty
  string falls through, an unset variable falls through.
- `${HOME:?HOME unset; …}` — if `$HOME` is also unset (e.g., a stripped
  CI environment), abort with a precise error rather than silently
  writing `/.human/last_fidelity_ab.json` at the filesystem root.
  `set -u` plus `${HOME:?…}` is the idiomatic shell guard.

### Edge cases

- **`$HOME` unset (CI minimal env).** The `${HOME:?…}` parameter
  expansion aborts the shell with the inline error message before any
  partial write. This is correct: a CI job that needs to run the
  orchestrator in a HOME-less env must explicitly opt in by exporting
  `HUMAN_FIDELITY_AB_PATH` or by passing `--no-publish`.
- **`~/.human/` does not exist (fresh user).** `mkdir -p "$(dirname
  "$dest")"` creates it idempotently. This matches the precedent in
  `scripts/merge-agent-metacog-config.sh:16` and avoids the ENOENT
  failure that a bare `mv` would hit.
- **`$dest` is a symlink to a file on a different filesystem.**
  `mv` will fall back to copy+unlink (non-atomic) in this case. Out
  of scope: `~/.human/` is canonically a regular directory; if a user
  has symlinked it across filesystems, they can pass
  `HUMAN_FIDELITY_AB_PATH=<path-on-same-fs>` or `--no-publish`.
- **Windows.** Out of scope. The repo is C+POSIX (see `AGENTS.md`
  §1: "C11 is the baseline … `fork`/`exec` on POSIX, guarded by
  `HU_GATEWAY_POSIX`"); shell scripts target macOS + Linux per the
  repo-standard `.cursor/rules/scripts.mdc`.

### Does `$HOME` need a `mkdir -p` of `.human`?

Yes — but **only the parent directory of `$dest`**, not `$HOME/.human`
specifically. We use `mkdir -p "$(dirname "$dest")"` so the same line
correctly creates `~/.human` (precedence 2) or any custom path's
parent (precedence 1, e.g. `HUMAN_FIDELITY_AB_PATH=/tmp/foo/bar.json`
needs `/tmp/foo`). This is one line and idempotent — cheap insurance.

## Atomicity guarantee

The required behavior (AC-B.2): the canonical file is either the
**previous** content or the **next** content; an external observer
can never `cat` a partial write.

The recipe:

```bash
tmpfile="$(mktemp "${dest}.XXXXXX")"
cp "$STATUS_JSON" "$tmpfile"
mv "$tmpfile" "$dest"
```

Why each line, and what guarantees come from where:

1. **`mktemp "${dest}.XXXXXX"`** creates the tmp file in the **same
   directory** as `$dest`. Same directory ⇒ same filesystem ⇒ `mv`
   is implemented as `rename(2)`, which POSIX guarantees is atomic
   ("if `newpath` already exists, the rename will atomically replace
   the existing file"). The `.XXXXXX` template is honored by both
   GNU `mktemp` (Linux) and BSD `mktemp` (macOS) — the repo's
   `mktemp -d "${TMPDIR:-/tmp}/lora-ab.XXXXXX"` at `lora-runner-ab.sh:120`
   already proves this template works on both platforms.
2. **`cp "$STATUS_JSON" "$tmpfile"`** does the actual write. If `cp`
   fails (disk full, permission denied), `set -e` aborts. The
   destination is untouched. The leftover tmp file is cleaned up by
   the existing `trap cleanup EXIT` only when `$CREATED_TMP=1`; in
   the persisted-output case the tmp file lingers next to `$dest`
   harmlessly until next cleanup. This is acceptable; we don't want
   to add a second trap that could race with the first.
3. **`mv "$tmpfile" "$dest"`** is the atomic flip. After this line
   the dashboard reads the new content; before this line it reads
   the old content (or sees ENOENT). No reader ever sees a half-
   written file.

### Alternative considered: `cp` with `>` + `mv`

```bash
cp "$STATUS_JSON" "${dest}.tmp" && mv "${dest}.tmp" "$dest"
```

This is what `scripts/merge-agent-metacog-config.sh:18–20` does. It
works but uses a **fixed** `.tmp` suffix, which:

- can collide if two `lora-runner-ab.sh` invocations race (rare in
  practice but cheap to defend against);
- can leak (and confuse a future reader) if a previous run was
  SIGKILL'd between `cp` and `mv` — the stale `.tmp` lingers with
  no random suffix to disambiguate it.

`mktemp "${dest}.XXXXXX"` costs one fork but is the right shape for
a published-state file. Either pattern is repo-precedented; we pick
the safer one.

### Failure mode if `mv` is cross-fs

`mv` falls back to copy+unlink, which is **not** atomic — a reader
between the copy and unlink would see a complete (but possibly
already-stale) file, never a truncated one. With the
same-directory `mktemp` we never hit this branch. We document the
constraint in a script comment so a future reader who changes the
tmp location knows why it must stay next to `$dest`.

## `--no-publish` flag

| Property | Value |
|---|---|
| Default | `0` (publish ON) |
| Where it's parsed | inside the `while [[ $# -gt 0 ]]` loop at `lora-runner-ab.sh:83`, alongside `--keep` |
| Where it short-circuits | the publish block, immediately after the existing `PASS` echo at line 189 |
| Effect on exit code | none — a `--no-publish` run still exits 0 on success |
| Effect on `$STATUS_JSON` | none — the file is still written to `--output-dir` |

CI/test path: a future smoke test (out-of-scope this story but worth
naming) can invoke the orchestrator in a sealed `HOME` with
`--no-publish` and assert exit code + presence of `--output-dir/status.json`,
then re-run *without* `--no-publish` against
`HUMAN_FIDELITY_AB_PATH=$tmpdir/test.json` and `diff` the two — that's
the AC-B.4 check.

## Failure-path guard

The orchestrator must **not** publish on any non-zero exit. The
existing structure already enforces this via `set -e` (line 66) plus
explicit `exit 1/2/3` calls. The publish block goes **after** every
existing failure check, so reaching it implies every prior step
returned 0. This is structural — no `if [ ${exit_code:-0} -eq 0 ]`
guard is needed because `set -e` makes "reach this line" semantically
equivalent to "exit code is 0 so far".

Specifically:

- **AC-B.5: exit-2 no-provider stub path.** When `human ml lora-runner`
  returns the all-empty `["",""…]` JSON (the symptom of an unconfigured
  cloud provider, an unbuilt huml backend, or `HU_IS_TEST` short-
  circuiting), the `empty_response_set` check at line 160 (BASE side)
  or 168 (ADAPTER side) calls `exit 2`. The publish block at the new
  line ~190 is unreachable. AC-B.5 holds by construction.
- **Argparse failure.** `usage 1` exits before any FS operation.
- **Adapter not found.** `exit 1` at line 101.
- **Comparator gate.** `exit 3` at line 180.
- **`fidelity-status` itself fails.** `set -e` propagates the non-zero
  from the subprocess; `trap cleanup EXIT` runs but cleanup only
  removes the (tmp) `OUTPUT_DIR`, never the canonical path.

The single explicit guard we add is the `${HOME:?…}` expansion — that
aborts before *any* mkdir/mv if both `HUMAN_FIDELITY_AB_PATH` is unset
**and** `HOME` is unset. Without that guard a stripped-env CI run could
attempt to write to `/.human/last_fidelity_ab.json` (root filesystem),
which is the exact "silently writes somewhere wrong" failure mode this
guard exists to prevent.

## Test plan

All tests are ad-hoc shell invocations the implementer (or a verifier)
can run in sequence. No new test files are required.

```bash
# Build the human binary once, all subsequent tests reuse it.
cmake --preset dev >/dev/null
cmake --build --preset dev --target human >/dev/null

# Sandbox HOME so we never touch the real ~/.human.
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/lora-ab-design-test.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT
mkdir -p "$SANDBOX/.human"

PERSONA_FIXTURE="tests/fixtures/lora_baseline_persona.json"
PERSONA_NAME="lora_baseline_fixture"
ADAPTER_FIXTURE="tests/fixtures/synthetic_lora.gguf"  # Story D may
# create this; for AC-B.5 only the no-provider exit-2 path matters,
# any non-empty file works as the --adapter argument because the
# all-empty BASE response triggers exit 2 before the adapter is ever
# loaded. If the synthetic fixture isn't present, use any 1-byte
# placeholder file:
#   ADAPTER_FIXTURE="$SANDBOX/fake-adapter.bin"; echo x >"$ADAPTER_FIXTURE"
cp "$PERSONA_FIXTURE" "$SANDBOX/.human/personas/${PERSONA_NAME}.json" \
  || { mkdir -p "$SANDBOX/.human/personas"; \
       cp "$PERSONA_FIXTURE" "$SANDBOX/.human/personas/${PERSONA_NAME}.json"; }
```

### AC-B.1 — diff(canonical, output-dir/status.json) is empty after success

```bash
# Hardest AC to exercise locally because it requires a provider that
# returns non-empty responses. Two viable approaches:
#   (a) point at a configured cloud provider (--provider openai with
#       OPENAI_API_KEY set);
#   (b) HU_IS_TEST=1 with a recorded fixture provider, if present.
# The verifier should pick whichever is wired. Pseudo-runnable form:
OUT="$(mktemp -d)"
HOME="$SANDBOX" \
  bash scripts/lora-runner-ab.sh \
    --persona "$PERSONA_NAME" \
    --adapter "$ADAPTER_FIXTURE" \
    --output-dir "$OUT"
diff "$SANDBOX/.human/last_fidelity_ab.json" "$OUT/status.json"
echo "AC-B.1 PASS: $?"   # expect 0 from diff (empty) and 0 from echo
```

If (a) and (b) are both unavailable in the verifier's env, AC-B.1 is
covered transitively by AC-B.4 (env-override write succeeds + diff
holds against the same `status.json`), since the only difference is
the destination path.

### AC-B.2 — `mv` is present, no direct redirect to canonical path

```bash
grep -n 'mv\s' scripts/lora-runner-ab.sh
# expect a line referencing `mv "$tmpfile" "$dest"`

# And there is NO direct write to the canonical filename anywhere:
! grep -nE '>\s*"?\$?\{?HOME[^"]*last_fidelity_ab\.json' scripts/lora-runner-ab.sh
! grep -nE '>\s*~/\.human/last_fidelity_ab\.json' scripts/lora-runner-ab.sh
echo "AC-B.2 PASS"
```

### AC-B.3 — `--no-publish` does not write the canonical file

```bash
# Pre-state: capture canonical path's stat (or absence).
PRE_STATE="$(stat -f '%m %z' "$SANDBOX/.human/last_fidelity_ab.json" 2>/dev/null \
  || stat -c '%Y %s' "$SANDBOX/.human/last_fidelity_ab.json" 2>/dev/null \
  || echo "absent")"

OUT="$(mktemp -d)"
HOME="$SANDBOX" \
  bash scripts/lora-runner-ab.sh \
    --persona "$PERSONA_NAME" \
    --adapter "$ADAPTER_FIXTURE" \
    --output-dir "$OUT" \
    --no-publish || true   # exit code may be 2 if no provider; we
                           # only care that the canonical file didn't
                           # change.

POST_STATE="$(stat -f '%m %z' "$SANDBOX/.human/last_fidelity_ab.json" 2>/dev/null \
  || stat -c '%Y %s' "$SANDBOX/.human/last_fidelity_ab.json" 2>/dev/null \
  || echo "absent")"
[ "$PRE_STATE" = "$POST_STATE" ] && echo "AC-B.3 PASS"
```

### AC-B.4 — `HUMAN_FIDELITY_AB_PATH` override writes to override path only

```bash
OUT="$(mktemp -d)"
OVERRIDE="$SANDBOX/test-ab.json"
rm -f "$OVERRIDE" "$SANDBOX/.human/last_fidelity_ab.json"

HOME="$SANDBOX" HUMAN_FIDELITY_AB_PATH="$OVERRIDE" \
  bash scripts/lora-runner-ab.sh \
    --persona "$PERSONA_NAME" \
    --adapter "$ADAPTER_FIXTURE" \
    --output-dir "$OUT"   # success required for this AC

test -f "$OVERRIDE" || { echo "AC-B.4 FAIL: override not written"; exit 1; }
test ! -f "$SANDBOX/.human/last_fidelity_ab.json" \
  || { echo "AC-B.4 FAIL: default path was also written"; exit 1; }
diff "$OVERRIDE" "$OUT/status.json" \
  && echo "AC-B.4 PASS"
```

### AC-B.5 — exit-2 no-provider path does NOT write the canonical file

The `human ml lora-runner` default provider falls through to the
configured chat provider; with `HOME=$SANDBOX` and no API keys
exported, the cloud providers will error every chat call and return
the all-empty JSON, which the script catches as exit 2.

```bash
rm -f "$SANDBOX/.human/last_fidelity_ab.json"
OUT="$(mktemp -d)"

# Strip provider env explicitly to guarantee the empty-response path.
( unset OPENAI_API_KEY ANTHROPIC_API_KEY GROQ_API_KEY MISTRAL_API_KEY \
        GEMINI_API_KEY GOOGLE_API_KEY VOYAGE_API_KEY OLLAMA_HOST
  HOME="$SANDBOX" \
    bash scripts/lora-runner-ab.sh \
      --persona "$PERSONA_NAME" \
      --adapter "$ADAPTER_FIXTURE" \
      --output-dir "$OUT" )
EXIT_CODE=$?
[ "$EXIT_CODE" -eq 2 ] || { echo "AC-B.5 setup FAIL: expected exit 2, got $EXIT_CODE"; exit 1; }
test ! -f "$SANDBOX/.human/last_fidelity_ab.json" \
  && echo "AC-B.5 PASS: canonical file not created on exit 2"
```

If the verifier's env happens to have provider creds that *do* respond,
the test should fall back to a deterministic exit-2 trigger by
pointing `--adapter` at a non-existent path *after* the file existence
check (currently line 101 exits 1, not 2 — so that won't work). The
cleanest deterministic exit-2 trigger is to set `HUMAN_BIN` to a
wrapper script that always emits `[]` from `ml lora-runner`:

```bash
WRAPPER="$SANDBOX/fake-human"
cat >"$WRAPPER" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *"ml lora-runner"*)
    out=""
    while [[ $# -gt 0 ]]; do
      [[ "$1" == "--output" ]] && { out="$2"; break; }
      shift
    done
    [[ -n "$out" ]] && echo '[""]' >"$out"
    exit 0
    ;;
  *) exit 0 ;;
esac
EOF
chmod +x "$WRAPPER"
HUMAN_BIN="$WRAPPER" HOME="$SANDBOX" \
  bash scripts/lora-runner-ab.sh \
    --persona "$PERSONA_NAME" \
    --adapter "$ADAPTER_FIXTURE" \
    --output-dir "$(mktemp -d)" \
  ; EXIT_CODE=$?
[ "$EXIT_CODE" -eq 2 ] && test ! -f "$SANDBOX/.human/last_fidelity_ab.json" \
  && echo "AC-B.5 PASS (deterministic wrapper path)"
```

This wrapper-based trigger is the recommended primary form because it
is hermetic — independent of the CI machine's provider config.

### AC-B.6 — `shellcheck` clean

```bash
shellcheck scripts/lora-runner-ab.sh
echo "AC-B.6 PASS: $?"   # expect 0
```

If `shellcheck` is not installed locally:

```bash
# macOS:  brew install shellcheck
# Linux:  apt-get install shellcheck   (or `nix profile install nixpkgs#shellcheck`)
```

The `${HOME:?…}` and the indirect `${HUMAN_FIDELITY_AB_PATH:-…}`
expansion should both be shellcheck-clean (SC2086 doesn't apply because
we always quote the result; SC2155 doesn't apply because we don't
combine `local` + assignment; the existing script already passes).

## Backward compatibility

Audited callers of `scripts/lora-runner-ab.sh` and the canonical path:

| Surface | Calls `lora-runner-ab.sh`? | Reads `last_fidelity_ab.json`? | Conflict with `--no-publish`? |
|---|---|---|---|
| `scripts/check-lora-ab.sh` | **No** — calls `human ml lora-ab` directly with fixture before/after files (`scripts/check-lora-ab.sh:67`). | No. | None. |
| `scripts/verify-all.sh` | No — only invokes `check-lora-ab.sh` via `run_check` (`scripts/verify-all.sh:97`). | No. | None. |
| `.github/workflows/*.yml` | No matches for `lora-runner-ab` or `last_fidelity_ab` or `HUMAN_FIDELITY_AB_PATH` in any workflow. | No. | None. |
| `src/gateway/cp_admin.c::cp_admin_metrics_fidelity` | n/a (reader, not caller). | **Yes** — `cp_admin.c:1142–1153`. | None — handler is read-only and tolerates the file's absence (emits `available:false`). |
| `tests/test_gateway_extended.c` | No — test sets `HUMAN_FIDELITY_AB_PATH` to a fixture and asserts the handler reads it. | Yes (via env override). | None. |
| `ui/src/demo-gateway.ts` | No — comments mention the script as the producer of the file, no execution. | No (mock data path). | None. |
| Docs (`docs/plans/*.md`) | Reference the script and path in prose only. | Reference in prose only. | None. |

**Net finding: `lora-runner-ab.sh` has no in-repo callers today.** The
only caller is the human operator. Adding `--no-publish` is therefore
a strict superset of the current CLI surface; no flag name collides
(`--keep` is the closest neighbor and it controls `OUTPUT_DIR`
deletion, not publishing). The new write to
`~/.human/last_fidelity_ab.json` is the file `cp_admin_metrics_fidelity`
already wants to read; the handler currently always finds it absent
and emits `available:false`. After this change the handler will start
finding it present and emit `available:true` — that's the entire point
of the story, and the dashboard's tile already handles both shapes
(see `cp_admin.c:1218` `available:false` branch).

## Risk + rollback

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| `mv` not atomic on the user's filesystem (cross-fs symlink, network FS quirks). | Low | Medium — partial file at canonical path. | Use `mktemp` in the same directory as `$dest` so `mv` always goes via `rename(2)`. Document the constraint in a one-line script comment. |
| `${HOME}` unset in CI silently falls through to `/`. | Low | Large — write to filesystem root, possibly with elevated perms in containers. | `${HOME:?HOME unset; pass --no-publish or set HUMAN_FIDELITY_AB_PATH}` parameter expansion aborts the script before any FS op. |
| Race between two concurrent `lora-runner-ab.sh` runs publishing to the same canonical path. | Very low | Low — one of the two writes wins, the dashboard reads a coherent file from one of them. | `mktemp` template gives each run a unique tmp path; `rename(2)` guarantees the loser's tmp file is replaced or unlinked, never interleaved. |
| `--no-publish` semantics drift (someone adds a code path that publishes anyway). | Low | Medium — tests would silently touch the user's `~/.human/`. | The publish block is the *only* write to `$dest`; `grep -n '\$dest' scripts/lora-runner-ab.sh` will list a single hit (the new block). A future-proofing assertion in AC-B.3 catches regressions. |
| Stale `~/.human/last_fidelity_ab.json` after a series of fast-failing runs misleads the dashboard. | Low | Low — file is replaced on the next *successful* run. | Out of scope: TTL/staleness display is a future Story-A enhancement. The existing `cp_admin_metrics_fidelity` already returns the file's `mtime` so the UI can flag age. |

**Rollback:** the patch is a single contiguous diff in one file. `git
revert <sha>` restores the previous state. `cp_admin_metrics_fidelity`
already tolerates the file's absence (it emits `{"available": false}`)
so a rollback does not break the dashboard, only stops refreshing it.

## Acceptance criteria mapping

| AC | Covered by | Evidence the verifier captures |
|---|---|---|
| AC-B.1 | publish block at end of script | `diff ~/.human/last_fidelity_ab.json $OUT/status.json` is empty after a successful run |
| AC-B.2 | `mv "$tmpfile" "$dest"` line | `grep -n 'mv\s' scripts/lora-runner-ab.sh` matches the new line; no direct redirect to canonical path |
| AC-B.3 | `if [[ "$NO_PUBLISH" -eq 0 ]]` guard | sandbox HOME stat unchanged after `--no-publish` run |
| AC-B.4 | `${HUMAN_FIDELITY_AB_PATH:-…}` expansion | only `$OVERRIDE` exists; `~/.human/last_fidelity_ab.json` does not |
| AC-B.5 | structural — publish AFTER all `exit 2`/`exit 3` checks | wrapper-driven exit-2 run leaves canonical path unchanged |
| AC-B.6 | shellcheck-clean expansions and quoting | `shellcheck scripts/lora-runner-ab.sh` exits 0 |

`RESULT_tech-lead=READY`
