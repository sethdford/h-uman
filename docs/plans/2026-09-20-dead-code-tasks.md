# Dead-code plan: execution tasks

Executes `docs/plans/2026-09-20-dead-code-plan.md` (the Spec: verdict,
inventory, phase order, Appendix A file classes, Appendix B function
clusters). Branch `worktree-dead-code-plan`, based on main `0f6fef169` plus
the merged `fix/channel-defines-reach-human`.

Exact input lists live in the SDD workspace
`.superpowers/sdd/2026-09-20-dead-code-tasks/lists/`:
`delete-modules.txt` (37), `merge-modules.txt` (36, every MERGE file except
`src/app/capabilities.c`), `keep-modules.txt` (19), `delete-functions.tsv`
(105 rows: symbol, defining file).

## Global Constraints

- C11, the project's `-Werror` flags. Build: `cmake --preset dev` once, then
  `cmake --build build --target human human_tests -j8`. Suite:
  `./build/human_tests` must exit 0 with zero failures. Do not run
  `cmake --build build` with no target (a known link failure in another target).
- Every task ends in a commit on this branch with message ending
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Never
  `--no-verify`. Pre-commit ratchets may rewrite baseline constants and stage
  them; include those in the commit.
- No new `HU_IS_TEST` logic forks. No `fprintf(stderr)` in `src/` (use
  `hu_log_*`). No new raw `getenv` for gates: use `hu_gate_mode_from_env`
  (`include/human/gate_mode.h`) with modes off|shadow|on.
- When deleting a `.c` file: also delete its header under `include/` if no
  surviving file includes it; remove its `CMakeLists.txt` lines (all targets,
  including `HU_TEST_EXTRA_*` lists); delete test files whose every
  reference is to deleted symbols; edit test files that mix deleted and live
  symbols to drop only the deleted parts; remove its `docs/CONCEPT_INDEX.md`
  row and any `docs/PRUNABLE_MODULES.md` / `docs/orphan-channels.md` row.
- Never touch files outside this worktree. Never `git stash`.
- Deleting dead code must not change the live symbol set: after any deletion
  task, `nm build/human | grep -c ' T _hu_'` equals the value recorded in the
  ledger before the task (the ledger holds it). If it changes, stop and
  report which symbol moved.
- Tests that assert a behavior must fail when the behavior is absent: no
  `ASSERT_TRUE(1)`, no `count >= 0`.

## Task 1: Merge the channel-define fix (controller)

Done by the controller: `git merge fix/channel-defines-reach-human`.
No subagent.

## Task 2: Wire exec-env sanitization into the shell tool

`src/doctor/doctor.c:472-474` prints "Exec env sanitization: active (blocks
MAVEN_OPTS, LD_PRELOAD, GLIBC_TUNABLES)". `hu_exec_env_sanitize`
(`src/security/exec_env.c`, header `include/human/security/exec_env.h`) has
no caller. `src/tools/shell.c:172-205` builds the child environment with bare
`setenv`.

1. Read `exec_env.c` and its existing tests (`grep -rl exec_env tests`).
2. In `shell.c`, at the point the child env is finalized (before exec),
   run the env through `hu_exec_env_sanitize` (or the equivalent API the
   header offers, e.g. build the envp array and pass it). Keep the existing
   allowlist behavior; sanitization removes the blocklisted names.
3. Add a test in the shell tool's test file: set `LD_PRELOAD=/tmp/x.so` and
   `MAVEN_OPTS=-Dx` in the parent, run the shell tool with a command that
   prints `env`, assert neither appears in the output and that `PATH` still
   does. If the shell tool cannot spawn under `HU_IS_TEST`, test the
   env-building helper directly: extract it as a static-linkage-free helper
   `hu_shell_build_child_env(...)` declared in `src/tools/shell_internal.h`,
   and assert pre (input contains LD_PRELOAD) / post (output does not).
4. Make the doctor line truthful automatically: have it read a
   `hu_exec_env_sanitize_enabled()`-style constant if one exists, otherwise
   leave the text as is now that it is true.

Commit: `fix(security): sanitize the shell tool child env; doctor claim was false`.

## Task 3: Make `agent.context_engine: "rag"` real

`src/app/bootstrap.c:1108-1121` parses `agent.context_engine`; the `"rag"`
value logs "not implemented; using legacy engine". `src/context/context_engine_rag.c`
(header `include/human/context/context_engine_rag.h`) implements the
context-engine vtable and has tests in `tests/` (grep `context_engine_rag`).

1. In the bootstrap branch for `"rag"`, call `hu_context_engine_rag_create`
   with the same allocator/config the legacy engine gets, and install it
   where the legacy engine is installed. Remove the "not implemented" log.
2. Add a bootstrap-level test (pattern: any existing test that builds a
   config and calls the bootstrap context-engine selection) asserting that
   `context_engine = "rag"` yields an engine whose vtable is the rag
   vtable, and that `"legacy"` still yields the legacy one.
3. `docs/` mention of the option (grep `context_engine`) must describe what
   rag does in one sentence.

Commit: `feat(context): context_engine "rag" selects the RAG engine instead of logging not-implemented`.

## Task 4: Wire the heartbeat engine

`README.md:251` lists a Heartbeat engine and `README.md:386` documents
`"heartbeat": {"every": "30m"}`. `src/observability/heartbeat.c` (header
`include/human/observability/heartbeat.h`, tests exist) exports
`hu_heartbeat_ensure_file` / `hu_heartbeat_tick`; nothing calls them.
`src/daemon/daemon_maintenance.c:82` has a `heartbeat_flush_due`-style tick.

1. Find how the config parser exposes `heartbeat.every` (grep
   `heartbeat` in `src/config/`). If it is not parsed, add the field to the
   config struct and parser with a duration string ("30m", "1h", "90s").
2. In `daemon_maintenance.c` at the tick, if the config has a heartbeat
   interval, call `hu_heartbeat_ensure_file` once at daemon start and
   `hu_heartbeat_tick` when the interval has elapsed (use
   `hu_time_wall_ms`, never `clock()`).
3. Test: pre/post contract. With interval 1 ms and a fake or real clock,
   two ticks produce a heartbeat file whose timestamp advanced. Without a
   configured interval, no file is created.
4. Update the README sentence to state what the file is and where it lives.

Commit: `feat(observability): heartbeat engine runs from the maintenance tick; README config key was inert`.

## Task 5: Expire pending facts from the decay path; fix the SC-8 citation

Part A. `include/human/memory/personal_model.h:391` says
`hu_personal_model_expire_pending_facts` is "called from the existing
decay-pruning path". It is not. `hu_personal_model_apply_decay` is at
`src/memory/personal_model.c:2734`, invoked from the Phase-4 prune at `:2709`.

1. Call `hu_personal_model_expire_pending_facts` inside
   `hu_personal_model_apply_decay` after the existing decay work, passing
   the same model/db handle. Log the count expired at debug level.
2. Test in the personal_model test file: insert a pending fact with an
   expiry in the past, run `hu_personal_model_apply_decay`, assert the
   pending-fact count went from 1 to 0. Insert one with a future expiry,
   assert it survives.

Part B. `docs/standards/security/threat-model.md:431` credits
`src/security/net_security.c` for SC-8 HTTPS enforcement. The reached path
is `src/tools/validation.c:330` (`hu_tool_validate_url`, called from
`src/tools/web_fetch.c:229`). Rewrite that row to cite `validation.c` and
the function. Do not touch `net_security.c` (Task 7 deletes it).

Commit: `fix(memory): pending facts expire from the decay path as the header claims; threat-model cites the live validator`.

## Task 6: Delete the 37 abandoned modules

Input: `lists/delete-modules.txt`. These are compiled into
`libhuman_core.a`, never linked into `human`, and no production `.c` names
any of their symbols (linker-proven 2026-09-20). Tests reference most of
them.

1. Write `scripts/dev/delete-modules.sh <list>` that, per file: `git rm`
   the `.c`; find its header(s) (`grep -rl "$(basename .c)" include/` and the
   private `src/**/*.h` next to it) and `git rm` any header no surviving file
   includes; strip its lines from `CMakeLists.txt` (every target and list);
   report test files that reference its exported symbols (`nm` is not
   available pre-build; use `grep -l` on the header's function names).
   Keep the script; it is reused by Task 7.
2. Run it. For each reported test file: delete it if every symbol it
   references is from deleted modules (check with grep against the
   surviving `include/`); otherwise remove only the dead test functions and
   their registration in `tests/test_main.c` or the suite file.
3. Remove the modules' rows from `docs/CONCEPT_INDEX.md`,
   `docs/PRUNABLE_MODULES.md`, `docs/orphan-channels.md` if present.
   `src/tunnel/none.c` and `src/tunnel/root.c` are both in the list; the
   `HU_ENABLE_TUNNELS` option and remaining tunnel backends stay.
4. Build and run the suite. Fix only breakage caused by the deletions
   (missing includes, dead test registrations, test-count guard baseline).
5. Report in the report file: files deleted, test files deleted, test
   files edited, and the before/after `human_tests` count.

Commit: `chore: delete 37 abandoned modules (linker-proven unreachable, no production caller)`.

## Task 7: Delete the 36 duplicate modules

Input: `lists/merge-modules.txt`. Each duplicates a live module (the twins
are listed in the Spec §4 P2 table); the classification found nothing worth
porting from any of them. Same procedure as Task 6 using
`scripts/dev/delete-modules.sh`.

Special cases:
- `src/memory/vector/chunker.c`: its prototypes live in the widely-included
  `include/human/memory/vector.h`; remove only those prototypes, keep the header.
- `src/behavior/rel_dynamics.c` is NOT in this list (it is a WIRE item);
  `src/context/rel_dynamics.c` is NOT in this list either (Task 21).
- `src/eval/longmemeval.c` twin is `src/evaluation/evaluation_longmemeval.c`;
  make sure the `human evaluation` CLI path still builds.
- `src/security/net_security.c`: the threat-model doc was already repointed
  in Task 5.

Commit: `chore: delete 36 duplicate modules whose live twins are reached from bootstrap/daemon/main`.

## Task 8: Fold capabilities.c into its two live callers, then delete it

`src/app/capabilities.c` (186 LOC, header `include/human/app/capabilities.h`)
is the better version of two hand-rolled implementations:
`src/app/cli_commands.c:1152` `cmd_capabilities` hardcodes tool/channel
strings, and `src/gateway/cp_admin.c:231` `cp_admin_capabilities` builds
JSON off `hu_channel_catalog_*`.

1. Read all three. Decide which of capabilities.c's behaviors are
   strictly better (catalog-driven lists instead of hardcoded strings, any
   field the others lack). Move that logic into a small shared helper that
   both callers use, placed in `src/app/capabilities.c` ONLY if both callers
   can link it (cli_commands.c is in `human`, cp_admin.c in `human_core`; a
   helper in human_core is reachable from both). Otherwise put it beside
   `channel_catalog.c`.
2. Make `cmd_capabilities` and `cp_admin_capabilities` call the helper;
   delete the now-unused remainder of capabilities.c and its header via
   `scripts/dev/delete-modules.sh`.
3. Tests: the existing tests for `cmd_capabilities` / `cp_admin` output
   must still pass; add one assertion that the CLI output lists a channel
   that exists in the catalog and is not a hardcoded literal.

Commit: `refactor(app): capabilities from the catalog in both CLI and RPC; delete the orphan copy`.

## Task 9: Delete 105 unreferenced functions

Input: `lists/delete-functions.tsv` (symbol, defining file). Every function
is dead-stripped by the linker and has zero textual references outside its
definition and header prototype (grep-verified 2026-09-20).

1. Write `scripts/dev/delete-functions.py` that, for each (symbol, file):
   locates the definition (a line at column 0 matching the C definition
   pattern for `symbol(` possibly with the return type on the previous
   line, followed by `{` on the same or next line), removes it through the
   matching closing brace using brace counting that ignores braces inside
   string/char literals and comments, and removes any preceding
   doc-comment block that immediately precedes it; then removes the
   prototype (and its doc comment) from any header under `include/` or the
   file's directory. Print what it could not find.
2. Run it. Five symbols have no header (listed in Spec Appendix B);
   `hu_world_model_invalidate_channel` is mentioned in
   `include/human/agent/world_model.h:468` only inside a doc comment; find
   its real prototype. Leave any `static` helper that becomes unused only if
   `-Werror=unused-function` complains, then delete that too.
3. Build and run the suite. Removing a function that a test references
   means the input list was wrong for that symbol: restore that one
   function, and report it.
4. Report: count removed, count restored, and the `nm` live-symbol count
   before and after (must be equal).

Commit: `chore: delete 105 unreferenced functions (dead-stripped, zero callers)`.

## Task 10: The dead-strip ratchet

Add `scripts/check-dead-strip-ratchet.sh`, same shape as
`scripts/check-clone-ratchet.sh` and registered like it in
`scripts/ratchet-config.tsv` and `.githooks/pre-push` (read both to match
the pattern exactly, including auto-lock via `scripts/lib/ratchet.sh`).

Measurement, in order:
1. Require `build/CMakeFiles/human.dir/link.txt` and
   `build/CMakeFiles/human_core.dir/link.txt` (skip with exit 0 and a
   message if the build dir is absent, matching how the other ratchets
   behave when their inputs are missing).
2. Relink `human` from `link.txt` with `-Wl,-dead_strip -Wl,-map,$TMP/human.map`
   to `$TMP/human_ds` (macOS) or `-Wl,--gc-sections -Wl,-Map` (Linux; if
   Linux map parsing is not implemented, exit 0 with "unsupported platform").
3. Count A = objects listed in `human_core.dir/link.txt` (archive members)
   none of whose global `T` symbols (`nm -g --defined-only`) appear in the
   map's live or dead-stripped sections. Attribute by symbol name, not by
   archive member basename.
4. Count B = `_hu_` symbols in the map's "Dead Stripped Symbols" section
   that no `human_tests` object references (`nm -u` over
   `build/CMakeFiles/human_tests.dir` and `human_core_test.dir`, cached in
   `$TMP`).
5. Fail if A or B exceeds its baseline constant; auto-lock the constants
   downward when measured below (same mechanism as the clone ratchet).
   Initial baselines: measure after Tasks 6-9 and write those values.
6. Print `A=<n> (never-loaded objects, ceiling X)` and
   `B=<n> (unreferenced dead functions, ceiling Y)`; list the names when
   failing.

Also: a test in `tests/` is not required for a shell script, but
`scripts/test-ratchet-lib.sh` exists; add a case there if it covers
per-ratchet scripts. Document the ratchet in the same doc that documents the
clone ratchet (grep `clone-ratchet` in `docs/`).

Commit: `ci: dead-strip ratchet (never-loaded objects, unreferenced dead functions) with auto-lock`.

## Task 11: Ship what you run: all-channels off in dev and prod presets

`CMakePresets.json` presets `dev` and `prod` currently inherit
`HU_ENABLE_ALL_CHANNELS=ON`. Only iMessage, Slack, Telegram, WhatsApp and
email are configured in production.

1. Set `HU_ENABLE_ALL_CHANNELS=OFF` in `dev` and `prod`, and explicitly
   enable `HU_ENABLE_IMESSAGE`, `HU_ENABLE_SLACK`, `HU_ENABLE_TELEGRAM`,
   `HU_ENABLE_WHATSAPP`, `HU_ENABLE_EMAIL` (and whatever `GMAIL`/`IMAP`
   options the email channel needs; check `bootstrap.c` `#if HU_HAS_*`).
2. Keep `HU_ENABLE_ALL_CHANNELS=ON` in the CI all-channels job
   (`.github/workflows/ci.yml`, grep `all-channels`) and in the `test`
   preset so `human_tests` keeps compiling every channel through
   `HU_TEST_EXTRA_CHANNELS`.
3. Verify: `cmake --preset dev` then build `human`; `nm build/human |
   grep -c ' T _hu_discord_'` is 0 and `grep -c ' T _hu_imessage_'` is > 0.
   Build `human_tests` with the dev preset and run it; it must still pass
   (channel tests compile via the test library).
4. Add `docs/build-options.md`: a table of every `HU_ENABLE_*` option
   (`grep -n '^option(HU_ENABLE_' CMakeLists.txt`), its default, which
   preset turns it on, and one line on what it gates. Generate it with a
   script (`scripts/dev/build-options-table.sh`) so it does not rot.

Commit: `build: dev/prod presets compile only the channels we run; document every HU_ENABLE_ option`.

## Task 12: Move the 19 keep-as-library modules out of the core archive

Input: `lists/keep-modules.txt`. These are test/eval/SDK library code,
opt-in backends (`memory/engines/postgres.c`, `redis.c`, `providers/llamacpp_*`),
and the portable `asm/generic/chacha20.c` fallback.

1. Add `add_library(human_devlib STATIC ...)` in `CMakeLists.txt` with the
   test/eval/SDK modules (`eval_rubric.c`, `m3_ab_fidelity_gate.c`,
   `stock_baseline.c`, `external_judge_fixture.c`, `user_sim.c`,
   `user_sim_scenario.c`, `dispatch.c`, `imessage_sticker.c`,
   `channel_manager.c`, `identity.c`, `runtime_bundle.c`, `mcp_tool_wrapper.c`,
   `persona_crypt.c`, `vision_ocr.c` only if `vision_ocr_apple.m` moves
   with it, else leave it). Link it into `human_tests`, `human_core_test`
   consumers, `human_synthetic` and the eval targets, not into `human`.
2. Leave option-gated backends (`postgres.c`, `redis.c`, `llamacpp_*`) and
   `chacha20.c` in `human_core` behind their existing `if(HU_ENABLE_*)` /
   arch conditions; they are not dead when their option is on.
3. `channel_manager.c` is a documented public SDK surface (`docs/api/channels.md`,
   `sdk/README.md`): add a comment at the top saying so, and a test that
   compiles `sdk/templates/channel/my_channel.h` against the header.
4. Build; `human_tests` passes; `human` links; the Task 10 ratchet's count A
   drops by the number of moved modules (record before/after in the report).

Commit: `build: human_devlib holds test/eval/SDK modules; the core archive is daemon code only`.

## Task 13: Resolve max_tokens per model in the request path

`src/agent/max_tokens.c` (`include/human/agent/max_tokens.h`, tests exist,
maintained through 2026-07-27) resolves a model's output cap. Nothing
populates `hu_chat_request_t.max_tokens`; providers fall back to constants
(`src/providers/anthropic.c:142`, `gemini.c:768`).

1. Where `hu_chat_request_t` is built in `src/agent/agent_turn.c` and
   `src/agent/agent_stream.c`, set `.max_tokens = hu_max_tokens_resolve(model)`
   (use the header's actual function name) when the request does not
   already carry a positive value. Gate with
   `hu_gate_mode_from_env("HU_MAX_TOKENS_RESOLVE")`: `off` = unchanged,
   `shadow` = log `would set max_tokens=<n> for <model>` and leave 0,
   `on` = set it. Default `shadow`.
2. Test: with the gate `on`, building a request for a known model yields the
   resolved value; with `off`, 0 (or the previous default); with `shadow`,
   0 and a log line.
3. Document the gate in the file that lists `HU_*` gate env vars (grep
   `HU_AGENT_FACTS` in `docs/` to find it).

Commit: `feat(agent): resolve max_tokens per model (HU_MAX_TOKENS_RESOLVE, default shadow)`.

## Task 14: Fill stop sequences from the registry

`src/agent/stop_sequence_registry.c` (`include/human/agent/stop_sequence_registry.h`)
per `docs/superpowers/plans/2026-05-14-output-validator-chain.md` P4.T19/T20.
`src/providers/anthropic.c:382` already sends `stop_sequences` when set;
nothing sets it.

1. At the same request-build sites as Task 13, populate `stop_sequences`
   from `hu_stop_sequence_registry_lookup(provider, channel)` (actual
   name per header) when empty. Gate `HU_STOP_SEQUENCES` off|shadow|on,
   default `shadow`.
2. Test: gate `on` → request carries the registry's sequences for
   (anthropic, imessage); `off` → empty.

Commit: `feat(agent): stop sequences from the registry (HU_STOP_SEQUENCES, default shadow)`.

## Task 15: Add the chain-of-thought audit validator to the reasoning chain

`src/agent/validators/cot_audit_validator.c` per the same plan, Task 11 /
P3.T17: "wire it ONLY into the chain that runs over reasoning_content".
The chain builder is `src/agent/validators/default_chains.c:35-52`
(`hu_validators_build_default_outbound_chain` and siblings).

1. Add `hu_validator_cot_audit_create` to the reasoning-content chain only
   (not the outbound text chain). Gate `HU_COT_AUDIT` off|shadow|on, default
   `shadow`: in shadow the validator runs and logs its verdict but never
   rejects.
2. Test: with `on`, a reasoning payload the validator is designed to
   reject is rejected; with `shadow`, accepted and a log line is emitted;
   the outbound text chain is unchanged (assert its validator count).

Commit: `feat(validators): CoT audit validator on the reasoning chain (HU_COT_AUDIT, default shadow)`.

## Task 16: Behavior-change selector in the turn

`docs/plans/2026-05-10-behavior-v1-followups.md:99`: "`hu_behavior_change_select`
(BCT recommender) is still not called from `agent_turn.c`". The live
sibling call is `hu_behavior_decide(&bin, &bdec)` at `src/agent/agent_turn.c:5052`.

1. Read `src/behavior/change.c`, its header and tests, and the followups
   doc §B4/B5/B14 to learn what the selector's output is meant to feed
   (a directive string into the prompt, per the doc).
2. Call it right after `hu_behavior_decide`, gated `HU_BEHAVIOR_CHANGE`
   off|shadow|on, default `shadow` (log the selected technique, do not
   inject). With `on`, inject the directive the way the followups doc says.
3. Test: pre/post contract with `on`: the prompt assembled for a fixture
   turn contains the directive; with `shadow`, it does not and a log line
   exists.

Commit: `feat(behavior): BCT change selector wired after hu_behavior_decide (HU_BEHAVIOR_CHANGE, default shadow)`.

## Task 17: Style mirror after generation, before send

`src/persona/style_mirror.c` (header already included at
`src/agent/agent_turn.c:33`, never called). Sprint 6 US-19: mirroring is
post-generation, pre-send. Design doc:
`docs/plans/2026-05-29-independent-taste/design.md`.

1. Find the post-generation, pre-send point in `agent_turn.c` (after the
   outbound validator chain runs; grep the chain call from Task 15).
   Apply `hu_style_mirror_*` there per the header's API. Gate
   `HU_STYLE_MIRROR` off|shadow|on, default `shadow` (log the would-be
   rewrite diff, send the original).
2. Test: with `on`, a fixture reply is rewritten as the module's own tests
   show; with `shadow`, the reply is unchanged and a log line exists.

Commit: `feat(persona): style mirror post-generation (HU_STYLE_MIRROR, default shadow)`.

## Task 18: Implement the follow-up watcher (shadow only)

`src/daemon/daemon_follow_up_watcher.c:47-79` is a stub: it `(void)`-discards
its arguments and comments "in a full version, this would: 1. call
hu_imessage_find_unreplied_read() ... 2. compute follow-up delay ...".
`hu_imessage_find_inbound_unreplied` (`src/channels/imessage.c`, header
`include/human/channels/imessage.h:117-121`) and `hu_follow_up_should_send_now`
(`src/agent/follow_up.c`, header `follow_up.h:188`) are the two halves.

1. Implement the tick: find inbound unreplied messages older than the
   configured threshold, ask `hu_follow_up_should_send_now` per candidate,
   and for each `yes` produce a follow-up proposal. Route the proposal
   through the SAME path proactive check-ins use (grep `proactive_send` and
   `init_proposer` in `src/daemon/`), so the throttle, reachability
   pre-filter and decision log apply. Never call a channel `send` directly
   from this file.
2. Gate `HU_FOLLOW_UP_WATCHER` off|shadow|on, **default `shadow`**: log
   `would follow up <contact> after <age>` and write a `proactive_decisions`
   row with `trigger='follow_up'` and `sent=0`. `on` is not to be flipped in
   this task.
3. Test under `HU_IS_TEST` with a chat.db fixture (pattern:
   `tests/test_imessage_chatdb_fixture.c`): one unreplied inbound older
   than the threshold, one replied, one newer. Assert exactly one
   candidate reaches `hu_follow_up_should_send_now`, and in shadow mode no
   send path is invoked (assert the channel mock's send count is 0).

Commit: `feat(daemon): follow-up watcher implemented behind HU_FOLLOW_UP_WATCHER (default shadow)`.

## Task 19: Cross-channel synthesis, daemon integration (spec Task 7)

`src/memory/cross_channel.c` (header `include/human/memory/cross_channel.h`,
2 tests) landed Tasks 1-6 of the cross-channel-synthesis spec (grep
`cross-channel` in `docs/plans` and `SESSION-HANDOFF.md`, WAVE 3). Task 7
was "replace the inline `cross_channel_ctx` builder" at
`src/daemon/daemon_reactive_context.c:158-475`.

1. Replace the inline builder with calls into `cross_channel.c`, preserving
   the exact output the inline code produced today. This is a
   behavior-preserving refactor: write a characterization test first that
   captures the inline builder's output for a fixture, then swap the
   implementation and keep the test green.
2. Delete the inline code. If `cross_channel.c` lacks a behavior the inline
   code had, add it to `cross_channel.c` (with a unit test) rather than
   keeping two implementations.
3. Update `SESSION-HANDOFF.md` WAVE 3 line to done.

Commit: `refactor(daemon): cross-channel context from memory/cross_channel.c (spec Task 7)`.

## Task 20: Route raw realpath() through hu_platform_realpath

`docs/standards/engineering/cross-platform.md:45-58` mandates
`hu_platform_realpath` (`include/human/platform.h:58`, `src/app/platform.c`).
Eleven live sites call raw `realpath()`: `src/tools/file_edit.c:127,159`,
`src/core/process_util.c:24`, `src/app/update.c:126`,
`src/platform/calendar_macos.c:53`, `src/doctor/doctor.c:1531`,
`src/agent/instruction_discover.c:32`, plus any others
`grep -rnw 'realpath(' src --include='*.c'` finds outside `platform.c`.

1. Replace each with `hu_platform_realpath` (same semantics: returns the
   resolved path or NULL; check the header for the buffer contract and
   adapt the call).
2. Add a `scripts/check-*.sh`-style guard (or extend an existing boundary
   check) that fails if a raw `realpath(` appears in `src/` outside
   `src/app/platform.c`; register it wherever the other check scripts run
   in pre-commit.
3. Existing tests cover these sites; run the suite.

Commit: `refactor(platform): all realpath through hu_platform_realpath; guard against regressions`.

## Task 21: Finish the rel_dynamics relocation

Commits `96444eda2` / `1f715f8ba` (2026-05-29) relocated relationship
dynamics to `src/behavior/rel_dynamics.c`, but
`include/human/daemon/context_facade.h:16` still includes
`human/context/rel_dynamics.h`, so the daemon uses the old copy in
`src/context/rel_dynamics.c` (Task 9 already removed its 8 dead functions).

1. Diff the two modules' remaining public APIs. Repoint
   `context_facade.h:16` (and any other includer of the context header) to
   `human/behavior/rel_dynamics.h`; adapt call sites in `src/daemon.c` and
   `src/daemon/daemon_reactive_prompt.c` if names differ.
2. Delete `src/context/rel_dynamics.c` and its header with
   `scripts/dev/delete-modules.sh`; move any test that only the old copy
   had onto the new module.
3. Build, suite, and the ledger's live-symbol count check (this task DOES
   change the live set: the old copy's live symbols are replaced by the new
   copy's; record both counts).

Commit: `refactor(behavior): finish rel_dynamics relocation; delete the context/ copy`.
