# Dead code in h-uman: inventory, verdict, and plan

Written 2026-09-20 against main `0f6fef169`. Companion to
`2026-09-20-october-roadmap.md` (product order) and the same-day
clean-architecture fleet review (structural order). This file owns one
question: **is the dead code a pathway to a state-of-the-art system or an
obstacle, and what do we do about it, in what order?**

Every number here was measured, not estimated. The oracle is the linker:
relink `human` with `-Wl,-dead_strip -Wl,-map`, attribute by symbol name,
take the object universe from `build/CMakeFiles/human_core.dir/link.txt`.
Grep-based counts were wrong twice on the way to these numbers (stale
`.o` files for moved sources; archive members named by basename while two
layers share module names). Do not re-measure with grep.

## 1. Verdict

**Obstacle, with a small pathway inside it.** Three findings drive that.

1. **Dead code here makes the codebase lie about itself.** Six places say a
   feature is active when the code that implements it is unreachable
   (§3). Wrong "it's wired" claims are the single most expensive failure
   mode this project has recorded (`verify-before-you-claim.md`: 30% of a
   session, twice), and dead-but-plausible modules are what generate them.
2. **It is being paid for.** 6 of the 107 orphan modules were edited in
   September by mechanical sweeps (paths refactor, dead-strip commit). That
   is Eder et al.'s ICSE 2012 result reproduced locally: dead methods keep
   absorbing maintenance changes, and about half of those changes are
   wasted. The orphan set is ~235K tokens of source plus 37K LOC of tests
   across 100 test files that every agent session can be routed into.
3. **But 28 modules (8.9K LOC) are the built-but-unwired half of features
   with a named integration point**, four with a written "not yet wired" in
   their own plan. That is the pathway part, and it is the *wiring*, not
   the code sitting in the archive. The October roadmap says the program is
   measurement-limited, not idea-limited, so only the wiring that serves
   O1-O5 or fixes a lie is worth doing now. The rest gets a decision date
   and is deleted if the date passes.

SOTA for this problem is not a one-time purge. Meta's SCARF deletes ~100M
LOC a year and Uber's Piranha removed 17% of feature flags because both
made deletion *continuous and automated*. The structural deliverable here
is therefore a linker-based ratchet in pre-push (§6), which the existing
`ratchet-config.tsv` auto-lock/decay machinery already supports.

## 2. Inventory (measured 2026-09-20)

| Class | Count | LOC | Notes |
|---|---|---|---|
| Core objects the daemon never loads | 146 files | 41.9K | of 1,039 archive objects with functions |
| ... of which HU_HAS_*-gated channels | 21 | 9.2K | build defect, fix in flight (§4 P0) |
| ... of which `#if`'d-out or dead-caller | 18 | 6.2K | classified below |
| ... of which **orphan modules** (no production `.c` names any symbol) | 107 | 26.5K | 98 untouched since May |
| Exported functions dead-stripped inside *linked* files | 965 | ~1 MB code | |
| ... referenced only by tests | 840 | | library surface; not this plan's target |
| ... referenced by nothing | 125 | | 113 grep-verified zero callers |
| CMake options OFF in dev and prod | 56 | | feature-flag debt |

Classification of the 125 never-linked non-channel files (107 orphan + 18
gated), by five read-only agents with verify-before-claim rules, exact
totals recomputed from their tables:

| Class | Files | LOC | Meaning |
|---|---|---|---|
| DELETE | 37 | 6,973 | abandoned or superseded; nothing planned depends on it |
| MERGE | 37 | 10,147 | duplicates a live module (twin named per file, Appendix A) |
| WIRE | 28 | 8,850 | intended half of a live feature; call site named |
| KEEP | 19 | 4,660 | test/eval/SDK library, opt-in backends, portable fallbacks |
| WIRE-PENDING-FIX | 3 | 1,695 | nostr/qq/signal, blocked on the HU_HAS_* fix |
| UNCONFIRMED | 1 | 430 | `agent/agent_routing.c`: producer of `session_key` not found |

The 113 zero-caller functions: **77 DELETE-NOW**, **29 DEAD-SUBSYSTEM**
(five clusters, each with a live twin), **4 WIRE**, 3 UNCONFIRMED, 0
public-API keeps (Appendix B).

## 3. The six lies (fix first, regardless of anything else)

| Where the claim lives | Claim | Reality |
|---|---|---|
| `src/doctor/doctor.c:474` | "Exec env sanitization: active (blocks LD_PRELOAD, ...)" | `hu_exec_env_sanitize` has no caller; `src/tools/shell.c:172-205` builds the child env with bare `setenv` |
| `docs/standards/security/threat-model.md:431` | SC-8 HTTPS enforcement credited to `security/net_security.c` | the reached path is `src/tools/validation.c:330`; net_security.c is never linked |
| `README.md:251,386` | Heartbeat engine + `"heartbeat": {"every":"30m"}` config key | `hu_heartbeat_tick` never called; daemon has its own ad-hoc 60 s cadence |
| `src/app/bootstrap.c:1108-1121` | accepts `agent.context_engine: "rag"` | logged "not implemented; using legacy engine"; `context_engine_rag.c` was complete and unlinked. **Fixed (task 3):** `"rag"` now installs the RAG context engine, which assembles context from a recent-message window plus memory-backed retrieval of the latest user message, instead of the legacy heuristic. |
| `include/human/memory/personal_model.h:391` | expire_pending_facts "called from the existing decay-pruning path" | it is not; `hu_personal_model_apply_decay` (`personal_model.c:2734`) never calls it |
| `CMakeLists.txt` `HU_ENABLE_ALL_CHANNELS=ON` | compiles 21 channels, bootstrap has `#if HU_HAS_X` blocks for them | target `human` received 11 of 44 defines; a Discord token in config was silently ignored. **Fixed on branch `fix/channel-defines-reach-human` (a6c3bc0de), unmerged** |

The first is a security claim on a tool that executes shell commands. It
is a one-line wire or a one-line doctor correction; either is fine, silence
is not.

## 4. The plan

Ordering rule: things that lie, then things that cost nothing to remove,
then duplicates, then decisions, then the ratchet that stops recurrence.
Deletions go **before** the `hu_agent_turn` / `hu_service_run` carves from
the clean-arch review: less code to carve, and the two do not conflict.
Everything touching `CMakeLists.txt` waits for the channel-define branch
to merge first.

### P0. Truth (≤1 day, one PR per row of §3)

- Merge `fix/channel-defines-reach-human`; reconfigure `build-prod`
  before the next deploy (prod config has no discord/matrix/signal/teams
  block, so nothing new starts).
- `exec_env`: call `hu_exec_env_sanitize` from `shell.c:172`, or delete
  the doctor line. Recommend wire: the blocklist is 190 LOC and tested.
- `net_security.c`: MERGE into `tools/validation.c` (P2) and fix the
  threat-model citation now.
- Heartbeat: wire `hu_heartbeat_tick` at `daemon_maintenance.c:82` or
  delete the README section and config key. Decide, don't defer.
- `context_engine: "rag"`: wire at `bootstrap.c:1108` (cheap, complete)
  or reject the config value with an error.
- `expire_pending_facts`: add the call inside `hu_personal_model_apply_decay`.
  This one serves roadmap item C/D (memory hygiene) directly.

### P1. Free deletions (1 day, scripted; no design decisions)

Per `agent-task-sizing.md`, N ≥ 20 identical edits is a script, not an
agent. Inputs are the file lists in Appendix A and B.

1. The 37 DELETE modules (6,973 LOC, 131 exported functions) plus the
   41 test files that reference only them, their headers under `include/`,
   their `CMakeLists.txt` source lines, and their `docs/CONCEPT_INDEX.md`
   rows. `tunnel/none.c` and `tunnel/root.c` go together; the whole tunnel
   family is `HU_ENABLE_TUNNELS=OFF` and listed in `docs/PRUNABLE_MODULES.md`.
2. The 77 DELETE-NOW functions and their header declarations (five have no
   header). `hu_world_model_invalidate_channel` appears in
   `world_model.h:468` only in a doc comment; find the real prototype.
3. The 29 DEAD-SUBSYSTEM functions: `voice/session.c` turn-taking half,
   `context/rel_dynamics.c` SQL + drift/repair (live twin `context/repair.c`),
   `context/intelligence.c` protective/humor/boundary (twin `context/protective.c`),
   gateway SSE trio, `gateway/oauth.c` trio (twin `security/auth.c`).
4. Expect the test-count guard, the clone ratchet baseline and
   `docs/STATS.md` to move; the ratchets auto-lock downward.

Gate: full suite green, `nm build/human | grep -c ' T _hu_'` unchanged
(deleting dead code cannot change the live symbol set; if it does, the
classification was wrong for that file, stop and look).

### P2. Merges (3-4 days; one PR per cluster)

37 files, 10,147 LOC, each with a verified live twin. Rule: delete the
orphan copy; port only behavior that is strictly better into the twin.
Clusters, largest first:

| Cluster | Orphans | Live twin | Port anything? |
|---|---|---|---|
| Vector stores | `vector/store.c`, `store_pgvector.c`, `store_qdrant.c` | `store_mem.c`, `vector_retrieval_remote.c` | no |
| Multimodal detectors | `multimodal/calendar.c`, `document.c`, `image.c` | `tools/calendar.c`, `tools/doc_ingest.c`, `multimodal.c` | no |
| ML training / DP | `ml/agent_trainer.c`, `ml/dp_sgd.c` | `ml/learner.c` + `scripts/*_mlx_train.py` | no |
| Memory lifecycle | `lifecycle/hygiene.c`, `migrate.c`, `summarizer.c`, `degradation.c`, `cognitive.c`, `engines/registry.c`, `graph/memory_graph.c` | `daemon_maintenance.c`, `memory/migration.c`, `consolidation_engine.c`, `forgetting.c`, `life_chapters.c` + `social_graph.c`, `memory/factory.c`, `memory/graph.c` | no |
| Retrieval | `memory/rag.c`, `retrieval/llm_reranker.c`, `vector/chunker.c` | `retrieval/engine.c`, `memory/rerank.c`, `tools/doc_ingest.c` | no |
| Persona | `persona/markdown_loader.c` (950), `persona/sticker.c` | `agent/agent_definition.c`, `context/conversation.c:7970` | no |
| Feeds | `feeds/apple.c`, `feeds/email.c` | `file_ingest.c` + `daily_feed_scrape.sh`, `feeds/gmail.c` | no |
| Agent | `activation_steering.c`, `service.c`, `state.c`, `subagent.c` | `agent_stream.c:1543`, `daemon.c`, `core/state_file.c`, `agent/spawn.c` | no |
| Surfaces | `app/capabilities.c`, `channels/channel_adapters.c`, `observability/status.c`, `peripherals/terminal.c` | `cli_commands.c:1152` + `cp_admin.c:231`, `channel_catalog.c`, `observability/health.c`, `design_tokens.h` | **yes**: capabilities.c is the better version, fold it into both callers |
| Security / MCP / misc | `security/mcp_audit.c`, `security/net_security.c`, `mcp/mcp_registry.c`, `skills/skills.c`, `sse/sse_client.c`, `util/http_util.c`, `eval/longmemeval.c` | `arg_inspector.c`, `tools/validation.c`, `mcp_manager.c`, `skillforge.c`, `providers/sse.c`, `core/http.c`, `evaluation_longmemeval.c` | no |

### P3. Decisions: the WIRE list (1 hour to decide, then per item)

These need Seth's call, not a default. Each row: wire it in October, or
delete it. Undecided by 2026-10-15 = delete (the roadmap's own rule: no
fifth mechanism before the fourth is measured).

**Serves the roadmap (recommend WIRE):**

| Module / function | Why it matters | Call site |
|---|---|---|
| `hu_imessage_find_inbound_unreplied` + `hu_follow_up_should_send_now` | the follow-up watcher is a literal stub; this is O2/O5 proactive reliability | `daemon_follow_up_watcher.c:73` — **wired (Task 18, 2026-09-21): `HU_FOLLOW_UP_WATCHER` (off\|shadow\|on via `hu_gate_mode_from_env`, default `shadow`) gates the real tick in `src/daemon/daemon_follow_up_watcher.c`. Both target functions are now reachable from the production binary (dead-strip unreferenced-symbol count 79 -> 77; `nm ' T _hu_'` 3440 -> 3442). OFF returns after the config/interval checks without querying chat.db. SHADOW runs detection + `hu_follow_up_should_send_now`, then logs `would follow up <contact> after <age>h` and writes ONE `proactive_decisions` row **per contact per UTC day** (`trigger='follow_up'`, `decision='defer'`, `reason='shadow:would_send'`, `sent=0`, `ts` in SECONDS); no send path is invoked. `defer` rather than `send`, and deduped per day, because `scripts/eval_when_to_speak.py:406` counts ANY `decision='send'` row in the window as "not missed" regardless of the `sent` column — a shadow row claiming `send` would mark a genuinely missed opportunity as caught and deflate MIR, and the predicate runs against a fresh throttle copy each tick so an undeduped standing proposal would emit ~288 rows/day. **Path choice:** the stub's own 4-step design was NOT followed. Step 3 (`follow_up_scheduled` table) does not exist anywhere in the repo, and step 4's `hu_daemon_follow_up_flush_for_contact` (`daemon_proactive.c:766`) fails the brief's condition — it calls `vtable->send` raw with no reachability pre-filter, no governor, no outbound sanitizer, records the throttle AFTER the send, writes no decision row, and sends `hu_autoresponder_build_prompt`'s PROMPT text verbatim to the contact. The `on` path instead runs `hu_daemon_proactive_reach_should_skip` (the O3 reachability pre-filter, `HU_PROACTIVE_REACHABILITY`/`blue_guard`) and then hands the proposal to `hu_daemon_proactive_gate_and_send`, the same arbiter proactive check-ins use (protective boundary, quiet hours + daily budget, reactive deferral, validator chain, sanitizer, channel rate-limit, send-cap, `send_and_record`). This file never calls a channel `send` itself. **`on` is deliberately inert in production** and is NOT flipped: the repo has no direction-correct follow-up copy. `hu_followup_compose_directive` (`followup_compose.c:66`) and `hu_followup_decide`'s `template_text` both phrase the OUTBOUND case ("<contact> read your last message and hasn't replied"), which is exactly backwards for this watcher's INBOUND case (they wrote, seth never answered) and would accuse a real person of ignoring a message they in fact sent. With no text source wired, `on` records the proposal and emits one `hu_log_warn_once` saying why. **Governor (fixed in review round 1):** the tick now takes `gov_budget` + `ar_cfg` and the daemon passes the SAME objects the check-in path uses (`&gov_budget`, `daemon_autoresponder_config()` at `src/daemon.c:~10266`, mirroring `:1587`). This matters because every governor gate is NULL-permissive: `hu_init_proposer_governor_check_only` skips quiet hours on `if (ar_cfg && …)` (`init_proposer.c:152`) and the budget on `if (budget && …)` (`:168`), and `hu_daemon_proactive_send_and_record` only debits `if (gov_budget)` (`daemon_proactive.c:1130`) — so the previous `NULL, NULL` made a follow-up neither limited by nor counted against the one shared daily proactive budget. **Flip prerequisite (only one left):** wire a direction-correct text source via `hu_daemon_follow_up_watcher_set_text_source`. If the text source, `gov_budget` or `ar_cfg` is missing at runtime, `on` degrades to shadow and a `hu_log_warn_once` names which. **Finder injection:** `hu_imessage_find_inbound_unreplied` is compiled out under `HU_IS_TEST` (`imessage.c:3235`), so no chat.db fixture can drive it inside the test binary. Rather than add an `HU_IS_TEST` fork, the finder is an injectable `hu_follow_up_finder_fn` whose DEFAULT is the real query; production never calls the setter. Age threshold is the header constant `HU_FOLLOW_UP_WATCHER_MIN_AGE_MS` (6h), not a config field, because `hu_follow_up_watcher_config_t` carries only `enabled` + `interval_seconds`. **Throttle note:** `hu_follow_up_should_send_now` is check-and-consume — it calls `hu_proactive_throttle_record_send`, which counts toward the same per-contact caps real check-ins use. The predicate therefore runs against a stack COPY of the throttle in every mode: charging the live ledger would suppress real sends in shadow, and in live it double-charged and tripped `gate_and_send`'s own send-cap (observed during TDD). `gate_and_send` stays the single authority that debits the ledger.** |
| `memory/cross_channel.c` | spec Tasks 1-6 landed, Task 7 "daemon integration" never did; replaces an inline builder | `daemon_reactive_context.c:158-475` |
| `agent/max_tokens.c` | still maintained; every provider falls back to a crude constant because `request->max_tokens` is never set | `agent_turn.c` / `agent_stream.c` request build — **wired (Task 13, 2026-09-21): `HU_MAX_TOKENS_RESOLVE` (off\|shadow\|on via `hu_gate_mode_from_env`, default `shadow`) gates a shared helper `hu_agent_internal_resolve_max_tokens` (`src/agent/agent.c`), called from both request-build sites right before the provider call; fills `req.max_tokens` only when still 0, never overrides an earlier cap (somatic/empathy/token-budget/S3 reroute). SHADOW logs `[max-tokens-resolve SHADOW] would set max_tokens=<n> for <model>` and writes nothing — zero production behavior change until flipped to `on`. No dedicated `HU_*` gate-env-var registry doc exists in this repo (checked); this row is the discovery point for this gate, same as `HU_AGENT_FACTS`'s entries in `docs/research/2026-09-02-august-2026-sota-gap-analysis.md` / `2026-09-05-sota-fleet-closing-report.md`.** |
| `behavior/change.c` | plan says verbatim "still not called from agent_turn.c" | `agent_turn.c:5052` |
| `validators/cot_audit_validator.c` | DELETED 2026-09-21: the audit is live via a direct call in agent_turn.c; the validator wrapper was a duplicate route | `validators/default_chains.c:35-52` |
| `agent/stop_sequence_registry.c` | providers honor the field; nothing fills it | `agent_turn.c` request build — **wired (Task 14, 2026-09-21): `HU_STOP_SEQUENCES` (off\|shadow\|on via `hu_gate_mode_from_env`, default `shadow`) gates a shared helper `hu_agent_internal_resolve_stop_sequences` (`src/agent/agent.c`), called from both request-build sites right after Task 13's `hu_agent_internal_resolve_max_tokens`; fills `req.stop_sequences`/`_count` only when empty, never overrides an earlier value. Registry lookup stays provider-only per `stop_sequence_registry.h`'s own contract — the helper takes `provider_name` (a plain string) plus the `agent` pointer for channel context (`agent->active_channel`/`_len`, the daemon-owned string already passed through unchanged elsewhere in these files, never interpreted by `src/agent/`); `agent` is folded only into the SHADOW throttle key/message, never into the registry lookup. (Signature note: `agent_turn.c` sits exactly at the file-size ceiling ratchet — `.claude/rules/file-size-ceiling.md` — so both call sites had to stay a true one-line addition; taking `agent` instead of separate channel+length args keeps the call short enough that clang-format doesn't wrap it, and a one-line whitespace trim elsewhere in `hu_agent_turn` offset the net LOC growth to zero.) SHADOW logs `[stop-sequences-resolve SHADOW] would set N stop sequence(s) for provider=<p> channel=<c>` at most once per distinct (provider, channel) pair per process and writes nothing — zero production behavior change until flipped to `on`. `req->stop_sequences` is a borrowed pointer straight into the registry's static arrays (never freed by caller on either side), so applying is a bare pointer + count assignment, no copy.** |
| `persona/style_mirror.c` | DELETED 2026-09-21: casing/punctuation are owned by the live style governor (hu_daemon_shape_text_inplace); partner-style mirroring (US-19) is a product decision to fold into the governor or drop | `agent_turn.c:33` |
| `hu_platform_realpath` | the mandated wrapper; 11 live sites bypass it with raw `realpath()` | `file_edit.c:127,159`, `process_util.c:24`, `update.c:126`, ... |
| `behavior/rel_dynamics.c` | half-finished relocation from `context/`; finish it, then P1 deletes the old copy | `include/human/daemon/context_facade.h:16` |

**Does not serve the roadmap (recommend DELETE unless Seth wants the feature):**

| Module | Feature | Cost to wire |
|---|---|---|
| `onboard/dispatcher.c`, `step_provider.c`, `step_welcome.c`, `onboard/state.c` | onboarding state machine (sprint 51/54) | 0.5 day; call sites named |
| `agent/a2a.c` | external agent-to-agent interop | 0.5 day |
| `doctor/ws_consumer.c` | `human doctor --watch` | 0.5 day |
| `gateway/cp_tasks.c` + `agent/task_store.c` | `tasks.*` RPC | 0.5 day, chained |
| `vector/vector_retrieval_remote.c` | Qdrant/pgvector backends | needs a config decision |
| `channels/imessage_private/client.c` | private-API send, gated OFF by design (#246) | phase state, keep gated |
| `tts/transcript_prep.c` (1,229) | SSML/emotion prep for Cartesia; stranded include at `cp_voice_stream.c:13` | 1 day |
| `config/config_mutator.c` (625) | allowlisted config writes with backup; both live write paths hand-roll it | 0.5 day; it is a safety layer |
| `agent/action_directives.c` | behind `HU_ENABLE_ACTION_LAYERS=OFF`, spec 2026-05-24 | flip the option |
| `security/vault_aead.c` | migration-plan Phase 2 consumer; `vault.c` (DELETE) is the deprecated XOR path | 1 day, or delete both |
| `agent/app_config.c` | DDD E4 narrow type; 6 callers still use the 25-param constructor | 0.5 day |
| `channels/channel_loop.c` + `agent/service.c` | pthread service wrapper; daemon drives its own loop | delete |
| `memory/cross_graph.c` | its named call site is `retrieval/multigraph.c`, which is itself DELETE | delete, or re-target to `retrieval/engine.c` |
| `lifecycle/cache.c` + `lifecycle/diagnostics.c` | chained orphans | delete |
| `memory/memory.c` erase/purge facade | GDPR-style erase; `human memory forget` uses v1 vtable | product decision |

**KEEP (19 files, 4,660 LOC)** stays but moves: postgres/redis engines,
llamacpp decode/sampling, chacha20 portable fallback, persona_crypt,
runtime_bundle, identity, vision_ocr (consumed by the `.m` backend),
eval_rubric, m3_ab_fidelity_gate, stock_baseline, external_judge_fixture,
mcp_tool_wrapper, user_sim + user_sim_scenario, channel_manager (public
SDK surface), dispatch and imessage_sticker (self-documented test
utilities). See P5.

### P4. Feature-flag debt (decide once)

56 `HU_ENABLE_*` options are OFF in dev and prod. With the define fix
merged, `HU_ENABLE_ALL_CHANNELS=ON` will now *link* 21 channels nobody
configures. Piranha's finding is that stale flags are debt, not
optionality. Pick one:

- **Ship what you run.** Default `HU_ENABLE_ALL_CHANNELS=OFF`; the dev and
  prod presets enable exactly iMessage, Slack, Telegram, WhatsApp, email.
  Everything else stays in the tree behind its option and compiles only in
  the all-channels CI job. Recommended: 9.2K LOC leaves the daily build.
- **Delete the channels** that have no config schema and no user
  (`qq`, `dingtalk`, `lark`, `google_rcs`, `google_chat`, `onebot`, `line`,
  `irc`, `tiktok`, `twitter`, `facebook`, `instagram`, `mattermost`,
  `teams`, `mqtt`, `twilio`, `nostr`) and keep Discord/Matrix/Signal
  behind options.

Either way, every remaining option gets an owner line in `CMakeLists.txt`
(what enables it, which preset turns it on, last date it was built).

### P5. Structure (1 day): the archive contains only daemon code

Move the 19 KEEP modules out of `libhuman_core.a` into a `human_devlib`
static library linked by `human_tests`, `human_synthetic` and the eval
targets only. After P1-P3 the never-loaded count for `human_core` should
be **zero**, which is what makes the ratchet in §6 a hard gate instead of
a decaying baseline. Public SDK surfaces (`channel_manager.c`) get a
comment saying so and a test that the SDK template still compiles.

## 5. What the research says, and what we can verify locally

Verified against primary sources on 2026-09-20 (venue, year, and the
specific number). Two things could not be verified and are marked.

| Source | Finding | Bears on |
|---|---|---|
| Eder, Junker, Jürgens et al., ICSE 2012 | 25% of methods unused; 7.6% of maintenance changes touched unused code; 48% of those changes were unnecessary | **reproduced here**: 6/107 orphan modules edited by September sweeps |
| Romano, Vendome, Scanniello, Poshyvanyk, IEEE TSE 46(1) 2020 | dead code is perceived as harmful to comprehension; developers leave it from fear of breakage and lack of tooling | why a linker oracle + ratchet beats "be careful" |
| Shackleton et al. (Meta), ESEC/FSE 2023 industry track | SCARF removed ~104M LOC in one year, >370K change requests over five years | SOTA = continuous automated deletion |
| Ramanathan et al. (Uber), ICSE-SEIP 2020 | Piranha removed 1,381 stale flags (17%); 65% of generated diffs landed unchanged | P4: 56 OFF options are the same debt |
| Rahman, Querel, Rigby, Adams, MSR 2016 | toggles speed release and accumulate as debt (39 Chrome releases) | P4 |
| Besker, Martini, Bosch, TechDebt 2019 | 23% of developer time wasted on technical debt (36% including indirect) | the cost side of carrying 27K LOC |
| Shi et al., ICML 2023 (PMLR 202) | LLM accuracy drops sharply with irrelevant context; under 30% of problems solved consistently with distractors | agent sessions routed into orphan modules |
| Xia, Deng, Dunn, Zhang, Agentless, 2024 | localization is the bottleneck: 34.3% line-level accuracy dominates the cost/accuracy trade-off | every dead module is a false localization target |
| Abal, Brabrand, Wasowski, ASE 2014 | 42 real `#ifdef` variability bugs in Linux; missing configurations and unintended interactions | 56 options × 1,777 `HU_IS_TEST` sites |
| Medeiros et al., IEEE TSE 2018 | 128 configuration-related bugs; 65% of disciplined-refactoring patches accepted | preprocessor cleanup is accepted when evidenced |
| Lehman's laws (increasing complexity, conservation of familiarity) | complexity grows unless work is spent reducing it | why a one-time purge regrows |
| Martin, Clean Architecture, 2017 (Dependency Rule; plugin boundaries) | boundaries are what make deletion safe | P5 is the boundary |
| Foote & Yoder, Big Ball of Mud, PLoP 1997 | dead code and promiscuous sharing are the diagnostic symptoms | the same-named-modules-in-two-layers finding |

Not verified: (a) any 2024-2026 paper quantifying dead code's effect on
coding-agent success on real repositories, the search found none with
numbers; (b) a function-length vs defect-density study with a citable
number (the search returned Landman et al. 2016 on cyclomatic complexity
vs SLOC, which is a different claim). Do not cite either.

The honest summary: the maintenance and configuration literature is solid
and reproduces here; the "LLM agents are hurt by dead code" claim rests on
the general distraction result (Shi) plus the localization result
(Agentless), not on a direct study. It is a strong inference, not a
measurement. If we want a measurement, the ratchet in §6 gives us the
before/after.

## 6. The ratchet (prevents recurrence)

`scripts/check-dead-strip-ratchet.sh`, same shape as
`check-clone-ratchet.sh` and registered in `ratchet-config.tsv`:

1. Relink `build/human` with `-Wl,-dead_strip -Wl,-map,$TMP/human.map`
   using `build/CMakeFiles/human.dir/link.txt` (≈2 s).
2. Count A = archive members from `human_core.dir/link.txt` with no symbol
   in either the live or dead-stripped sections (never loaded).
3. Count B = `_hu_` symbols in the dead-stripped section not referenced
   by any `human_tests` object (`nm -u`, cached per build).
4. Fail on growth of A or B past the baseline; auto-lock downward;
   weekly decay target per `ratchet-config.tsv`.

Baselines today: A = 146, B = 125. Targets: A = 0 after P5, B < 20.
Runs in pre-push (the suite already takes ~4 min; this adds seconds).

**As built (2026-09-21, Task 10, on `4d376689b`).** Shipped as
`scripts/check-dead-strip-ratchet.sh`, wired into **both** hooks — `pre-commit`
for the auto-lock (the only place `ratchet_autolock` can rewrite and stage a
baseline) and `pre-push`, which rebuilds `build/` incrementally and then
enforces in strict mode — and registered as the
`dead-strip-objects` / `dead-strip-symbols` rows of
`scripts/ratchet-config.tsv` (floors 0 and 20, from the targets above). The
gate is documented in `.claude/rules/dead-strip-ratchet.md`, beside
`.claude/rules/clone-ratchet.md`. Measured after Tasks 6-9 landed:
**A = 50, B = 99** (down from the 146 / 125 above), in **~1 s** warm.

Three measurement rules the steps above do not state, each of which was wrong
before it was right (details in the rule doc):

- A member is counted in A only if it exports **≥1 global `T` symbol**. 56 of
  the 1,024 members export none — the `data_prompts_*_txt.c.o` blobs and
  `outbound/{strip,shape,echo,persona,moderation}.c.o`, which carry only `D`/`S`
  stage tables. Counting them would inflate A by 56.
- **Common symbols (`nm` type `C`) are excluded** from the liveness test. ASan
  gives every object a common `____asan_globals_registered`, which the linker
  coalesces into one map entry; testing it by name marks all 1,024 members live
  and collapses A to 0.
- B counts **symbols, not functions**: 40 function symbols + 59 function-local
  statics (`_hu_fn.DEFAULT_LEN`, `_hu_fn.sql`) at this baseline.

Two limits, both deliberate: the baselines are specific to `build/`'s
configuration (`build-check` compiles a different set of translation units), and
the map parser is macOS `ld`'s, so the gate prints `RATCHET_SKIP` and exits 0 on
Linux, on a missing build dir, or when `build/` is older than `src/`.

## 7. Do not

- Do not delete the 840 test-referenced dead functions in linked files as
  a batch. They are a per-file API-surface question, and the tests that
  reference them are the only spec those functions have.
- Do not wire anything in P3 "because it is built". The roadmap is
  measurement-limited; unwired mechanism is the cheapest thing to delete.
- Do not touch `HU_IS_TEST` forks here; the clean-arch review owns that.
- Do not re-audit the KEEP list; the reasons are in Appendix A.
- Do not trust this document past 2026-10-20 without rerunning §6 step 1.

## Appendix A. All 125 never-linked non-channel files

Columns: file, LOC, class. Evidence and twins per row are in the agent
tables archived in the 2026-09-20 session; the class is the decision.

| file | LOC | class |
|---|---|---|
| `src/agent/action_preview.c` | 141 | DELETE |
| `src/agent/agent_profile.c` | 76 | DELETE |
| `src/agent/case_based.c` | 252 | DELETE |
| `src/agent/prompt_optimizer.c` | 206 | DELETE |
| `src/agent/workspace_context.c` | 448 | DELETE |
| `src/calibration/ab_compare.c` | 83 | DELETE |
| `src/channels/channel_http.c` | 42 | DELETE |
| `src/channels/maixcam.c` | 184 | DELETE |
| `src/channels/webhook.c` | 276 | DELETE |
| `src/feeds/google.c` | 156 | DELETE |
| `src/feeds/music.c` | 177 | DELETE |
| `src/feeds/oauth.c` | 108 | DELETE |
| `src/gateway/tenant.c` | 147 | DELETE |
| `src/intelligence/skill_system.c` | 355 | DELETE |
| `src/memory/lifecycle/diagnostics.c` | 101 | DELETE |
| `src/memory/lifecycle/rollout.c` | 60 | DELETE |
| `src/memory/lifecycle/snapshot.c` | 205 | DELETE |
| `src/memory/multimodal_index.c` | 157 | DELETE |
| `src/memory/retrieval/multigraph.c` | 257 | DELETE |
| `src/memory/retrieval/qmd.c` | 183 | DELETE |
| `src/memory/retrieval/query_expansion.c` | 148 | DELETE |
| `src/memory/vector/outbox.c` | 105 | DELETE |
| `src/memory/vector/provider_router.c` | 118 | DELETE |
| `src/observability/multi_observer.c` | 86 | DELETE |
| `src/providers/mlx.c` | 888 | DELETE |
| `src/security/replay.c` | 116 | DELETE |
| `src/security/sandbox.c` | 10 | DELETE |
| `src/security/vault.c` | 518 | DELETE |
| `src/tools/canvas_render.c` | 294 | DELETE |
| `src/tools/cron_session_tools.c` | 377 | DELETE |
| `src/tools/lsp.c` | 29 | DELETE |
| `src/tools/mcp_resource_tools.c` | 192 | DELETE |
| `src/tunnel/none.c` | 84 | DELETE |
| `src/tunnel/root.c` | 46 | DELETE |
| `src/util/portable_atomic.c` | 87 | DELETE |
| `src/util/sse_parser.c` | 216 | DELETE |
| `src/voice/opus.c` | 45 | DELETE |
| `asm/generic/chacha20.c` | 92 | KEEP |
| `src/behavior/user_sim.c` | 83 | KEEP |
| `src/behavior/user_sim_scenario.c` | 54 | KEEP |
| `src/channels/channel_manager.c` | 81 | KEEP |
| `src/channels/dispatch.c` | 179 | KEEP |
| `src/channels/imessage_sticker.c` | 68 | KEEP |
| `src/eval/external_judge_fixture.c` | 74 | KEEP |
| `src/eval/stock_baseline.c` | 87 | KEEP |
| `src/mcp/mcp_tool_wrapper.c` | 71 | KEEP |
| `src/memory/engines/postgres.c` | 867 | KEEP |
| `src/memory/engines/redis.c` | 1101 | KEEP |
| `src/ml/m3_ab_fidelity_gate.c` | 205 | KEEP |
| `src/persona/eval_rubric.c` | 334 | KEEP |
| `src/persona/persona_crypt.c` | 808 | KEEP |
| `src/providers/llamacpp_decode.c` | 49 | KEEP |
| `src/providers/llamacpp_sampling.c` | 189 | KEEP |
| `src/providers/runtime_bundle.c` | 40 | KEEP |
| `src/security/identity.c` | 71 | KEEP |
| `src/tools/vision_ocr.c` | 207 | KEEP |
| `src/agent/activation_steering.c` | 161 | MERGE |
| `src/agent/service.c` | 102 | MERGE |
| `src/agent/state.c` | 255 | MERGE |
| `src/app/capabilities.c` | 186 | MERGE |
| `src/channels/channel_adapters.c` | 37 | MERGE |
| `src/eval/longmemeval.c` | 220 | MERGE |
| `src/feeds/apple.c` | 636 | MERGE |
| `src/feeds/email.c` | 181 | MERGE |
| `src/mcp/mcp_registry.c` | 214 | MERGE |
| `src/memory/cognitive.c` | 327 | MERGE |
| `src/memory/degradation.c` | 428 | MERGE |
| `src/memory/engines/registry.c` | 286 | MERGE |
| `src/memory/graph/memory_graph.c` | 499 | MERGE |
| `src/memory/lifecycle/hygiene.c` | 156 | MERGE |
| `src/memory/lifecycle/migrate.c` | 240 | MERGE |
| `src/memory/lifecycle/summarizer.c` | 166 | MERGE |
| `src/memory/rag.c` | 321 | MERGE |
| `src/memory/retrieval/llm_reranker.c` | 86 | MERGE |
| `src/memory/vector/chunker.c` | 130 | MERGE |
| `src/memory/vector/store.c` | 204 | MERGE |
| `src/memory/vector/store_pgvector.c` | 348 | MERGE |
| `src/memory/vector/store_qdrant.c` | 299 | MERGE |
| `src/ml/agent_trainer.c` | 501 | MERGE |
| `src/ml/dp_sgd.c` | 373 | MERGE |
| `src/multimodal/calendar.c` | 260 | MERGE |
| `src/multimodal/document.c` | 187 | MERGE |
| `src/multimodal/image.c` | 168 | MERGE |
| `src/observability/status.c` | 80 | MERGE |
| `src/peripherals/terminal.c` | 148 | MERGE |
| `src/persona/markdown_loader.c` | 950 | MERGE |
| `src/persona/sticker.c` | 428 | MERGE |
| `src/security/mcp_audit.c` | 150 | MERGE |
| `src/security/net_security.c` | 390 | MERGE |
| `src/skills/skills.c` | 85 | MERGE |
| `src/sse/sse_client.c` | 431 | MERGE |
| `src/subagent.c` | 448 | MERGE |
| `src/util/http_util.c` | 66 | MERGE |
| `src/agent/agent_routing.c` | 430 | UNCONFIRMED |
| `src/agent/a2a.c` | 350 | WIRE |
| `src/agent/action_directives.c` | 182 | WIRE |
| `src/agent/app_config.c` | 56 | WIRE |
| `src/agent/max_tokens.c` | 242 | WIRE |
| `src/agent/stop_sequence_registry.c` | 46 | WIRE |
| `src/agent/task_store.c` | 511 | WIRE |
| `src/agent/validators/cot_audit_validator.c` | 55 | DELETED |
| `src/behavior/change.c` | 163 | WIRE |
| `src/behavior/rel_dynamics.c` | 333 | WIRE |
| `src/channels/channel_loop.c` | 61 | WIRE |
| `src/channels/imessage_private/client.c` | 225 | WIRE |
| `src/config/config_mutator.c` | 625 | WIRE |
| `src/context/context_engine_rag.c` | 331 | WIRE |
| `src/doctor/ws_consumer.c` | 609 | WIRE |
| `src/gateway/cp_tasks.c` | 305 | WIRE |
| `src/memory/cross_channel.c` | 332 | WIRE |
| `src/memory/cross_graph.c` | 266 | WIRE |
| `src/memory/lifecycle/cache.c` | 256 | WIRE |
| `src/memory/vector/vector_retrieval_remote.c` | 683 | WIRE |
| `src/observability/heartbeat.c` | 260 | WIRE |
| `src/onboard/dispatcher.c` | 134 | WIRE |
| `src/onboard/state.c` | 167 | WIRE |
| `src/onboard/step_provider.c` | 367 | WIRE |
| `src/onboard/step_welcome.c` | 117 | WIRE |
| `src/persona/style_mirror.c` | 189 | DELETED |
| `src/security/exec_env.c` | 190 | WIRE |
| `src/security/vault_aead.c` | 566 | WIRE |
| `src/tts/transcript_prep.c` | 1229 | WIRE |
| `src/channels/nostr.c` | 575 | WIRE-PENDING-FIX |
| `src/channels/qq.c` | 338 | WIRE-PENDING-FIX |
| `src/channels/signal.c` | 782 | WIRE-PENDING-FIX |

## Appendix B. The 113 zero-caller functions, by cluster

| Cluster | File(s) | Functions | Class |
|---|---|---|---|
| Voice turn-taking | `voice/session.c` | activity_start/end, agent/user_turn_signal, last_action, note_interrupt_silence, note_response_complete/first_byte, recv_event, send_tool_response (10) | DEAD-SUBSYSTEM (twin `voice/realtime.c`) |
| rel_dynamics SQL + drift/repair | `context/rel_dynamics.c` | drift_detect, budget_multiplier, create_table_sql, insert_sql, query_sql, velocity_deinit, repair_should_activate, repair_state_deinit (8) | DEAD-SUBSYSTEM (twin `context/repair.c`) |
| protective/humor/boundary | `context/intelligence.c` | protective_{create_table,insert,query}_sql, boundary_deinit, humor_build_directive (5) | DEAD-SUBSYSTEM (twin `context/protective.c`) |
| Gateway SSE | `gateway/gateway.c` | send_sse_headers/chunk/end (3) | DEAD-SUBSYSTEM |
| Gateway OAuth | `gateway/oauth.c` | oauth_init/destroy/session_valid (3) | DEAD-SUBSYSTEM (twin `security/auth.c`) |
| Agent-pool binders | `agent/spawn.c` | bind_fleet_cost_tracker, set_team_config, set_worktree_manager | DELETE-NOW |
| Agent config/scene/tokens | `agent/agent.c`, `agent/context.c`, `agent/task_list.c` | estimate_tokens ×2, from_app_config, set_scene_direction, set_task_list, task_list_all | DELETE-NOW (from_app_config: see P3 app_config) |
| enum→string helpers | 6 files | behavior_risk_name, conflict_resolution_str, query_category_str, retrieval_strategy_str, weakness_type_name, write_outcome_str | DELETE-NOW |
| deinit/free twins | ingest, skill_trust, undo | ingest_result_deinit, skill_audit_entry_deinit, undo_entry_free | DELETE-NOW |
| static init/cleanup twins | commitment, fast_capture, relationship | *_data_init / *_data_cleanup (6) | DELETE-NOW |
| tool/provider setters | browser_use, computer_use, message, openai, hula | set_grounding ×2, set_channel, set_ws_streaming, set_delegate_registry | DELETE-NOW |
| reaction wire wrappers | `daemon/daemon_reaction_poll.c` | wire_collector, wire_personal_model | DELETE-NOW (daemon.c:2171/2180 wires directly) |
| memory facade erase | `memory/memory.c` | facade_erase, purge_by_provenance | UNCONFIRMED (product decision) |
| platform realpath | `app/platform.c` | platform_realpath | **WIRE** (11 bypass sites) |
| platform/config trivia | platform.c, config_merge.c | get_home_env, parse_datetime, config_env_get | DELETE-NOW |
| pending-fact expiry | `memory/personal_model.c` | expire_pending_facts | **WIRE** (`apply_decay`, :2734) |
| world-model bridge | `agent/world_model_bridge.c` | w14_register_belief_reverify (UNCONFIRMED), w7_facade_graph_db (DELETE-NOW) | mixed; the real gap is that nothing enqueues the job |
| directive builders | anticipatory, circadian, conversation ×2, goals, behavioral, narrative_self, behavior_trust | 8 unadopted variants of live builders | DELETE-NOW |
| security extras | audit, delegation, skill_trust | 6 unadopted siblings | DELETE-NOW |
| observability extras | background_registry, cost, observability | 3 | DELETE-NOW |
| channel-catalog predicates | `channels/channel_catalog.c` | contributes_to_daemon, requires_runtime | DELETE-NOW |
| superseded `_ex` twins | message_router, world_model | dispatch_imessage_reply_msg, invalidate_channel | DELETE-NOW |
| LLM-backed variants | learner, evaluation_frontier_compare, planner, swarm | 4 | DELETE-NOW |
| vector/retrieval extras | embeddings, retrieval/engine, store_sqlite_vec, strategy_learner | 4 | DELETE-NOW |
| feeds extras | `feeds/processor.c` | item_provenance, semantic_search | DELETE-NOW |
| follow-up watcher halves | `agent/follow_up.c`, `channels/imessage.c` | should_send_now, find_inbound_unreplied | **WIRE** (`daemon_follow_up_watcher.c:73`) |
| graph/memory scoring | graph ×3, promotion, reflection/storage | 5 | DELETE-NOW |
| MCP client lifecycle | `mcp/mcp.c` | reconnect, refresh_tools | DELETE-NOW |
| throttle reset, PWA tab, channel extras | proactive_throttle, pwa/bridge, telegram_reactions, thread_binding | 4 | DELETE-NOW |

Totals: DELETE-NOW 77, DEAD-SUBSYSTEM 29, WIRE 4, UNCONFIRMED 3.
Header-less exports (one-file delete): `hu_agent_internal_set_scene_direction`,
`hu_config_env_get`, `hu_conversation_build_group_member_directive`,
`hu_telegram_handle_reaction_update`, `hu_weakness_type_name`.
