# Critic findings — US-14.3 (iOS archive + IPA export + UUID leak guard)

Branch: impl/US-14.3  
Commit: e8efd689  
Critic run: 2026-05-17

---

## HIGH (2)

- `scripts/ios-archive-export.sh:111-115` — `SKIP_EXPORT=1` exits before the `plutil` bundle-ID and version assertions (lines 139-158). In CI the provisioning secret is absent, so EVERY PR run takes the `SKIP_EXPORT=1` path and reports `SUCCESS (archive only)` without ever exercising the acceptance criterion "plutil confirms bundle ID and version match expected" (AC-14.3.3). The plutil assertions only fire when a real provisioning secret is present, which is not the normal CI path. Fix: run the plutil assertions against the `.xcarchive`'s own `Info.plist` (available even without export) so the check fires on every run, not only on a signed export.

- `scripts/ios-archive-export.sh:78` — the entire substitution block (`cp .bak`, `sed -i`, `rm .tmp`) has its stderr redirected to `/dev/null`. If `cp` or `sed` fails (e.g. read-only filesystem, disk-full, EXPORT_OPTIONS_SRC path wrong), the block silently succeeds, `EXPORT_OPTIONS_BAK` is never written, cleanup() finds no `.bak` file and skips the restore, and the real secret values remain in `ExportOptions.plist` in the working tree. The post-run CI step at `native-apps-fleet.yml:207` re-runs the leak guard and would catch this only if the workflow has not already exited — but if `set -euo pipefail` caused an earlier step to exit first, the `if: always()` step would still run and catch it. However on a local developer run there is no such backstop. Fix: remove `2>/dev/null` and wrap the block in an explicit error check (`|| { echo "substitution failed"; exit 1; }`).

---

## MED (2)

- `scripts/check-no-provisioning-leak.sh:43-44` — the UUID regex `[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}` catches only the canonical dashed format. `IOS_PROVISIONING_UUID` supplied by the operator could be entered without dashes (32 hex chars, some tools emit this format) or uppercased differently. `sed` would substitute it verbatim into the plist; the leak guard would not detect it on the next commit because no dashes are present. The guard would also fail to detect a base64-encoded UUID. Fix: add a no-dash UUID check (`[0-9a-fA-F]{32}`) to the regex and document in the script comment that operators must supply the dashed canonical form.

- `.githooks/pre-commit:115-125` — the pre-commit leak guard only fires when `apps/ios/ExportOptions.plist` is itself in the staged diff (`--diff-filter=ACM`). A developer who runs `ios-archive-export.sh` locally, SIGKILL-interrupts it after substitution but before the EXIT trap fires, and then stages an unrelated file will not trigger the guard. The modified `ExportOptions.plist` with real secrets sits in the working tree as an unstaged change and the next `git add -p` or `git add .` could sweep it in on a future commit without the guard seeing it (because that future commit's diff would show the plist as newly modified). Fix: the guard should also run when the plist is present and MODIFIED in the index regardless of whether the current commit is the one that touched it — i.e., drop the `--diff-filter=ACM` scoping and always check the staged content when the plist is staged.

---

## LOW (1)

- `scripts/ios-archive-export.sh:73-76` — `sed -i.tmp` on macOS creates a backup at `$EXPORT_OPTIONS_SRC.tmp`; the script deletes it at line 77. But `EXPORT_OPTIONS_SRC` already has a `.plist` suffix, so the tmp file is `ExportOptions.plist.tmp`. If `.gitignore` does not cover `*.plist.tmp`, a failed run that exits before line 77 leaves a `.tmp` file with real secret values that `git status` will show as untracked. It is not committed (untracked), but it is on disk in plain sight. Confirm `apps/ios/ExportOptions.plist.tmp` is covered by `.gitignore`; add the pattern if not.

---

## Cross-agent regression risk

None detected. The changed files (`scripts/ios-archive-export.sh`, `scripts/check-no-provisioning-leak.sh`, `apps/ios/ExportOptions.plist`, `.githooks/pre-commit`, `.github/workflows/native-apps-fleet.yml`) are not touched by any other in-flight worktree visible in this repo. The pre-commit hook addition is additive (appended to existing hook); it does not remove or reorder existing checks.

---

RESULT_critic=HAS_FINDINGS story=US-14.3 severity=HIGH
