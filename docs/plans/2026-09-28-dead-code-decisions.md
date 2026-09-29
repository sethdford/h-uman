# Dead code: the §P3/§P4 decisions

Written 2026-09-28 against `origin/main` `e218ed5a5`. Closes the open rows of
`2026-09-20-dead-code-plan.md` §P3 (WIRE list) and §P4 (feature-flag debt)
ahead of that plan's 2026-10-15 "undecided = delete" deadline.

Every reference count below was re-measured on this commit, not copied from
the 2026-09-20 plan. Three rows changed since then (noted inline).

## Principle

The plan's §7 rule holds: **do not wire anything because it is built.** A
module is wired only when a live call site needs it today *and* the wiring
can ship behind the OFF→SHADOW→LIVE contract
(`.claude/rules/feature-gate-requires-measurement.md`). Everything else is
deleted. Git history is the archive; a deleted module is one `git revert`
away if a measured need appears.

## Decisions

| Module | LOC | Decision | Why |
|---|---:|---|---|
| `onboard/{dispatcher,state,step_provider,step_welcome}.c` | 785 | **DELETE** | No src caller; untouched since 2026-05-25. |
| `agent/a2a.c` | 350 | **DELETE** | External agent interop; no caller, no roadmap item. |
| `doctor/ws_consumer.c` | 609 | **DELETE** | `doctor --watch` never wired. |
| `gateway/cp_tasks.c` + `agent/task_store.c` | 816 | **DELETE** | Chained orphans. `tests/test_cp_tasks.c` actually tests `task_store`, not `cp_tasks`. |
| `memory/vector/vector_retrieval_remote.c` | 683 | **DELETE** | Remote Qdrant/pgvector retrieval contradicts local-first; no caller. |
| `security/vault_aead.c` | 566 | **DELETE** | Live secrets already use AES-256-GCM (`security/secrets.c`). The XOR vault it was meant to replace is gone. The migration plan's real open item, key storage (its Phase 3), belongs to `secrets.c`. **Follow-up F3.** |
| `agent/action_directives.c` + `HU_ENABLE_ACTION_LAYERS` | 182 | **DELETE** | Compile-time flag, OFF since 2026-05-24. It changes outbound prompts but has no runtime OFF/SHADOW/LIVE gate and no measurement. |
| `channels/channel_loop.c` | 61 | **DELETE** | The daemon drives its own loop. (`agent/service.c` was already gone.) |
| `memory/cross_graph.c` | 266 | **DELETE** | Its named call site was already deleted. |
| `memory/lifecycle/cache.c` | 256 | **DELETE** | Orphan; untouched since 2026-03-10. |
| `memory/cross_channel.c` | 332 | **DELETE** | The library ACL denies everything because `origin_relationship_type` is never populated. The right design (below) needs provenance data that does not exist yet; unwired code that denies all is worse than none. |
| `behavior/change.c` | 163 | **DELETE** | Needs 11 inputs, and the turn can derive 1. Fabricating the rest would make its safety gates vacuous in code that texts real people. |
| `behavior/rel_dynamics.c` | 333 | **DELETE** | An unused redesign. The live relationship prompt uses `context/rel_dynamics.c`. DDD placement is fixed by *moving the live file* into `behavior/` (a pure relocation, E1-style), never by switching the prompt to the unused API without a characterization test. |
| `memory.c` `hu_memory_facade_{erase,purge_by_provenance}` | ~60 | **DELETE the facade functions; design F1** | Only one backend implements the hook (graph entities, `memory_v1_backend.c:364`). The `memories` table, embeddings and summaries are not reached, and matching is by **substring**, so purging `contact:ann` also purges `contact:anna`. Wiring this behind a "forget" command would be a false privacy claim. |
| `agent_routing` `identity_links` session scoping | — | **Do not wire; make it observable (F2)** | Declared in `hu_session_config_t` and advertised by `human config` help, but it has **no parser**: its test suite is deliberately unregistered ("parsers not implemented (PR #115)", `tests/test_main.c`). So a configured block was silently ignored. The session scoping it would feed is reachable only from tests. Linking people across channels merges audiences. See F2. |
| `tts/transcript_prep.c` | 1246 | **KEEP** (changed since 2026-09-20) | Now wired from `tts/speech_direction.c` and `tts/voice_reply.c`; edited 2026-09-27. |
| `config/config_mutator.c` | 625 | **KEEP; wire (F4)** | A safety layer: allowlisted config writes with backup. Both live write paths hand-roll it, and a bad write silently disables subsystems (`.claude/rules/silent-config-gated-subsystems.md`). This is a real safety gain, not "because it is built". |
| `agent/app_config.c` | 56 | **KEEP** | It is the DDD E4 narrow config type; E4 migrates the 6 callers of the 25-parameter constructor. |

**§P4, feature-flag debt: ship what you run.** The owner's config enables
exactly two channels (`imessage`, `pwa`). The `dev` and `release` presets
build those; a new `all-channels` preset keeps every other channel compiling
in its own CI job. Changing `dev` moves every configuration-specific ratchet
baseline, so this lands as its own PR with re-measured baselines. **F5.**

## Follow-ups (designed work, not cleanup)

- **F1: provenance-exact erasure (DDD E3).** Deletion must cascade from a
  source through every derived artifact: memories rows, graph entities,
  embeddings, summaries and caches. Match provenance **exactly**, never by
  substring, and write an audit record kept separate from erasable memory.
  This is Yao et al., *Forgetting Without Restarting: Execution-State
  Unlearning for Stateful LLM Agents*, arXiv 2609.04875 (2026-09-04), whose
  provenance-guided selective replay reaches reset-equivalent forgetting.
  E3's repository layer is where the cascade belongs: one repository per
  aggregate, each implementing `erase_by_provenance`.
- **F2: audience-bounded context (prerequisite for `identity_links` and
  cross-channel memory).** Record the audience with every memory at write
  time. At prompt-assembly time, include an item only if every current
  viewer was in its recorded audience. Links between identities must be
  **user-declared**, never inferred. This is Liu, *Authorization Before
  Context: A Model-Neutral Audience Boundary Against Cross-Audience Memory
  Leakage in Agentic Systems*, arXiv 2608.17148 (2026-08-17). Until this
  exists, `session` gets the same nested-key validation as `gateway`,
  `memory` and `voice`: `session.identity_links` is reported as an unknown
  key (an error under `HUMAN_STRICT_CONFIG`), and the CLI help no longer
  advertises it.
- **F3: secret key storage.** Confirm where `secrets.c` keeps the AES key.
  If it is on disk next to the ciphertext, adopt the migration plan's
  Phase 3 (OS keychain) for the live path.
- **F4: route config writes through `config_mutator`.**
- **F5: ship-what-you-run presets** (above).

## Evidence method

`ld -dead_strip` + link map on the `dev` preset, the same oracle as
`scripts/check-dead-strip-ratchet.sh`, plus a repo-wide word-match of every
candidate symbol across `src/ include/ tests/ apps/ fuzz/`, excluding
`build*/` and worktrees. Every DELETE row was then re-checked by hand for
callers outside its own file. Research claims are cited only where the
source was fetched and confirmed; unverifiable sources were dropped.
