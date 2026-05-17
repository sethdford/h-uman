---
status: complete
last_audit: 2026-05-17
---

# Adversarial Review — SOTA-2026 Initiative Designs

**Reviewer**: critic subagent  
**Date**: 2026-05-11  
**Scope**: 14 design documents (master coordinator + 13 per-initiative designs)  
**Sprint under review**: SOTA-2026-01 (init-09, init-04, init-14, init-01, init-11-typing)

---

## Summary Table

| # | Severity | Initiative(s) | One-Line Description |
|---|----------|---------------|----------------------|
| B1 | BLOCKER | #09, #10 | Trust-tier ordinal direction inverted between the two designs |
| B2 | BLOCKER | #09 | Migration assigns FIRST_PARTY to pre-existing poisoned memories |
| B3 | BLOCKER | #09 | `hu_personal_model_ingest` signature change has ≥4 unpatched call sites |
| B4 | BLOCKER | Codebase | `hu_episode_t` already defined twice — live ODR violation |
| M1 | MAJOR | #09 | MINJA detector: 10 English-only patterns, common false-positive risk |
| M2 | MAJOR | #04 | No recovery path for in-flight response when Python subprocess crashes |
| M3 | MAJOR | #04 | 30s `chat_timeout_ms` kills healthy helper on long generations |
| M4 | MAJOR | #01 | SAE training cost glossed over — weeks of compute, not a sprint task |
| M5 | MAJOR | #05 | TTT DPO pair trains on synthesized preferred-response, not observed |
| M6 | MAJOR | #05, #04 | TTT adapter format contract unspecified — `load_adapter` may reject |
| M7 | MAJOR | #02 | `preferred_slot` boost applied pre-softmax; semantics demand post-softmax floor |
| M8 | MAJOR | #07 | `HU_ENABLE_ML` release binary is 10.1 KB vs declared 8 KB ceiling |
| M9 | MAJOR | #14 | `claude-opus-5-20260301` is a fabricated model ID — violates AGENTS.md §10.1 |
| N1 | MINOR | #12 | Synthesis precondition covers one rename; init-12 needs two |
| N2 | MINOR | #09 | `HU_TRUST_*` prefix collides with existing constants in two unrelated headers |
| N3 | MINOR | #09 | Quarantine log uses `%s` for user-controlled content without JSON escaping |
| N4 | MINOR | #11 | Dispatcher integration file ambiguous (`dispatcher.c` vs `channels/dispatch.c`) |
| N5 | MINOR | #02 | `macro_mode_floor=0.0` silently breaks "macro adapter always present" invariant |
| N6 | MINOR | #10 | W7 cleanup labelled "optional in S1" but is mandatory before S2 |
| T1 | NIT | #09, master | `agent_stream.c` P0 line numbers stale in both docs (design says 345, master says 370; actual is 370 per `Read`, but grep found a second call site at ~line 2460 neither doc acknowledges) |
| T2 | NIT | #01 | "No public surface additions" claimed, but new `include/human/persona/steering.h` IS a public header |
| T3 | NIT | #14 | `eval-reproduce.sh` claims reproducibility but KnowU-Bench dataset URL has no pinned commit hash |

---

## BLOCKER Findings

### B1 — Trust-tier ordinal direction inverted between init-09 and init-10

**Initiatives**: #09, #10  
**Design doc refs**: `init-09-memory-trust-tiers.md §2.1`, `init-10-episode-storage-sleep-consolidation.md §4`

**Problem**: Init-09 defines `hu_trust_tier_t` with USER_DIRECT=4 (highest) and UNTRUSTED=0 (lowest). Init-10's `hu_episode_t` struct declares `uint32_t trust_tier` with an inline comment of "0=user-direct, 1=persona-derived, 2=tool, 3=third-party" — the exact opposite ordinal direction. Because init-09 is shipping in S1 and its enum values will be committed to a public header, any S2 code that reads init-10's `trust_tier` field and compares or routes through init-09's `hu_trust_can_overwrite` logic will apply inverted filtering: what init-09 treats as high-trust (value 4) maps to "third-party" in init-10's encoding. The bug will be silent — no compile error, no assertion, just wrong trust decisions at runtime.

**Fix**: Align on a single canonical ordinal direction before init-09 ships. Recommended: keep USER_DIRECT=4 (init-09's scheme) as the semantic model because higher integer = higher trust is more intuitive in comparisons (`if (src.tier >= threshold)`). Update init-10's `uint32_t trust_tier` field to `hu_trust_tier_t trust_tier` (typed, not raw uint32_t) so a future mismatch becomes a compiler warning. Add a static assertion in `trust.h`: `_Static_assert(HU_TRUST_USER_DIRECT > HU_TRUST_UNTRUSTED, "trust tier ordering invariant")`.

---

### B2 — Init-09 migration grants FIRST_PARTY trust to pre-existing poisoned memories

**Initiatives**: #09  
**Design doc ref**: `init-09-memory-trust-tiers.md §2.8`

**Problem**: The SQLite migration sets `trust_tier = 2` (FIRST_PARTY) for all existing memory rows with the justification "benefit of the doubt — we don't know the source but it was probably legitimate." This reasoning is backwards for a security migration. The entire motivation for init-09 is that existing memories may already be poisoned (MINJA/MemoryGraft). An attacker who injected content before the migration is shipped sees their payload upgraded from "no tier" to FIRST_PARTY — the second-highest trust level, above THIRD_PARTY, sitting just below PERSONA_DERIVED. FIRST_PARTY means init-09's `merge_facts_checked` will apply the rule "first-party facts require explicit user confirmation to overwrite" — making the poisoned facts *harder* to dislodge, not easier. The migration makes poisoned memories more privileged than honest third-party data.

**Fix**: Set the migration default to `trust_tier = 1` (THIRD_PARTY) or `trust_tier = 0` (UNTRUSTED). THIRD_PARTY is the correct reading of "we received this over some unverified channel." Any memory the user has explicitly confirmed can be re-elevated to USER_DIRECT or PERSONA_DERIVED via the confirmation path that init-09 already defines. If the operational concern is that legitimate memories will be quarantined, add a one-time "trust elevation wizard" as part of the first-run UX post-migration, not a blanket tier-2 grant.

---

### B3 — `hu_personal_model_ingest` signature change patches 2 of ≥4 call sites

**Initiatives**: #09  
**Design doc ref**: `init-09-memory-trust-tiers.md §3.1 "P0 call-site fixes"`, master coordinator synthesis "Apply Init #09's P0 fixes"

**Problem**: Init-09 adds `const hu_provenance_t *prov` as a new required parameter to `hu_personal_model_ingest`. The design lists two P0 patches: `agent_stream.c:345` and `agent_turn.c:951`. Ground-truth grep of the live codebase confirms the actual call sites are:
- `src/agent/agent_stream.c:370` (user message ingestion)
- `src/agent/agent_stream.c:~2460` (a second ingestion call — streaming final content — that neither the init-09 doc nor the master coordinator mentions)
- `src/agent/agent_turn.c:951` (correct)
- `src/memory/personal_model.c` (2 matches — likely self-referential internal calls, but must be verified)

The signature change without patching every call site produces a C compiler error (`too few arguments`). Sprint S1 cannot produce a green build unless all call sites are found and updated. The design doc's line number of 345 is wrong for the first stream call (actual: 370), suggesting the site-survey was done against a different snapshot of the code than what is currently in `main`. More critically, the second `agent_stream.c` call site — which may be ingesting the *assistant* response text — has different provenance semantics from the user message ingestion and must be tagged deliberately.

**Fix**: Run `grep -rn "hu_personal_model_ingest" src/` against the actual branch at sprint kickoff. Confirm and enumerate every call site. The second `agent_stream.c` occurrence should receive `HU_TRUST_FIRST_PARTY` provenance with `from_assistant=true`. The `personal_model.c` occurrences must be audited: if they call `ingest` as an internal re-ingestion loop, they may need to pass provenance through from the caller. Gate the sprint on a full call-site sweep before writing any S1 code.

---

### B4 — `hu_episode_t` is already doubly-defined in the live codebase — ODR violation in effect

**Initiatives**: #10 (and transitively #05, #07, #02 which depend on episode fields)  
**Header refs**: `include/human/agent/episodic.h:22`, `include/human/memory/deep_memory.h:23`

**Problem**: The live codebase contains two incompatible struct definitions both named `hu_episode_t`:
- `include/human/agent/episodic.h` (session-summary format: `summary`, `summary_len`, `timestamp_ms`, `session_id`)
- `include/human/memory/deep_memory.h` (deep-memory format: `participants`, `participants_len`, `occurred_at`, `source_tag`)

These are different structs with different fields. In C, including both headers in the same translation unit is undefined behavior (ODR violation via `typedef` redefinition). The file `include/human/memory/episodic.h` acknowledges this with a NOTE comment — meaning the collision is known and unresolved. Init-10 proposes adding a THIRD `hu_episode_t` (verbatim conversation turns). The master coordinator calls W7 cleanup "optional in S1" but this is wrong: the existing dual definition is an ODR violation that is already defect-present in the build. Any S1 or S2 code that cross-includes will fail or silently pick the wrong struct layout. W7 is mandatory, not optional.

**Fix**: Promote W7 to a required pre-S1 gate: rename `agent/episodic.h`'s type to `hu_session_episode_t` and `memory/deep_memory.h`'s type to `hu_deep_episode_t` as the synthesis already proposes. Update all callers. Add a compile-time guard (`#ifdef HU_EPISODE_T_DEFINED / #define HU_EPISODE_T_DEFINED` or a simpler include-guard protocol) to catch future double-inclusion. This is a one-day fix — do it before any other S1 code.

---

## MAJOR Findings

### M1 — MINJA detector is 10 case-insensitive English-only patterns with high false-positive rate

**Initiative**: #09  
**Design doc ref**: `init-09-memory-trust-tiers.md §2.6`

**Problem**: The `hu_minja_detect` implementation is described as "10 hand-curated case-insensitive patterns" bounded to 512 bytes. The patterns are English-only ("your name is now", "from now on", "actually I want you to", etc.). Three categories of bypass exist: (1) non-English injection ("von nun an bist du Bob", "désormais tu t'appelles", "a partir de ahora"), (2) paraphrase injection ("going forward you should respond as", "consider yourself renamed"), and (3) false positives on legitimate messages ("you are now connected", "from now on I'll handle it", "actually I want you to help me plan"). The design acknowledges "English-only v1" only in a parenthetical, not as a known limitation in the risk register. For a feature being positioned as the defense against a real threat class, a system that bypasses with any non-English input is a brittle first line — and the false positives will cause user friction.

**Fix**: Scope the S1 deliverable honestly. Document in the risk register that the MINJA detector is an English-only heuristic with known bypass paths, not a comprehensive defense. The real defense is the trust-tier enforcement in `merge_facts_checked` — the MINJA detector is belt-and-suspenders, not the primary security control. Reframe it accordingly in the executive summary so users and auditors don't rely on it. For v2, replace pattern matching with an embedding-distance classifier: compute cosine similarity between the candidate message and a "persona-override instruction" centroid, which is language-agnostic.

---

### M2 — MLX subprocess crash mid-stream: partial response is silently orphaned

**Initiative**: #04  
**Design doc ref**: `init-04-mlx-qwen3-provider.md §7.3 "Subprocess state machine"`, §5.3

**Problem**: The state machine correctly moves the helper to `FAILED` and schedules resurrection on the *next* turn. But what happens to the *current* turn? If the Python process crashes after sending partial token output (only possible in a future streaming implementation, but the protocol design already reserves the `stream` opcode for v2), the C provider returns an error. The user sees either a truncated response or an error with no explanation. More immediately: with synchronous chat (v1), the `recv_framed_json` call will block until `chat_timeout_ms` (default 30s) fires before detecting the crash. During this 30s window the agent appears frozen. The design says "resurrection attempted on next turn" but the UX consequence — a frozen 30-second wait followed by a hard error — is not surfaced in the risk register.

**Fix**: Add dead-process detection before the timeout expires. After each `send_framed_json`, `poll()` the pipe FD with a short-interval (100ms) check loop rather than blocking on a single 30s recv. If the pipe is closed (EOF), transition to FAILED immediately and return `HU_ERR_PROVIDER_UNAVAILABLE` within one poll cycle. This reduces the "apparent freeze" from 30s to <200ms. Also add a test case `mlx_qwen3_helper_crash_mid_turn_returns_error_immediately` under `HU_IS_TEST`.

---

### M3 — `chat_timeout_ms` default of 30s terminates a healthy helper on long-context generations

**Initiative**: #04  
**Design doc ref**: `init-04-mlx-qwen3-provider.md §6.1 "Configuration"`, §7.3

**Problem**: `chat_timeout_ms` defaults to `0 → 30000`. For Qwen3-4B at 4-bit quantization on Apple Silicon, generating 512 tokens takes approximately 4–12 seconds on M1 Pro and 8–25 seconds on M2 Air. The 30s limit is within striking distance of the normal generation time for a long-context or long-max-tokens request. A user who asks for a detailed explanation and sets `max_tokens=800` will intermittently hit the timeout, see the helper transition to `FAILED`, and trigger an unnecessary backoff+restart cycle. The helper is killed for being slow, not broken. The state machine transitions to `FAILED` after a timeout, which is indistinguishable from a crash.

**Fix**: Decouple the "helper is dead" timeout from the "generation is taking too long" timeout. Use two separate watchdogs: (1) a keepalive ping (every 5s) to detect a truly dead process, and (2) a per-request budget (e.g., `max_tokens * 150ms` for an adaptive ceiling) that returns a partial result rather than killing the helper. Alternatively, store `last_recv_ts` and consider the helper alive as long as bytes are arriving. Do not fire the FAILED transition if the pipe FD is still producing data.

---

### M4 — SAE training cost is glossed over — "we'll just train an SAE on persona banks" is weeks on Apple Silicon

**Initiative**: #01  
**Design doc ref**: `init-01-activation-steering.md §3.2 "SAE path"`, §7 Risk Register (R5)

**Problem**: Risk R5 says "SAE training time on-device: budgeted 12-16 GPU-hours on M2 Max." This is dramatically optimistic for a Sparse Autoencoder on Qwen3-4B activations. SAE training on a 4B model requires iterating over the full residual stream at every layer, which for a modestly sized persona example bank of 10,000 turns would require approximately 10K × 4096-dim × 2 bytes ≈ 80 MB of activations per layer × 32 layers. A single training pass is manageable, but SAEs require thousands of epochs to converge with good feature specificity (the Anthropic SAE training used ~1B tokens). The design's persona bank has "hundreds to thousands of examples." An SAE trained on hundreds of examples will overfit, producing noise rather than meaningful steering vectors. The 12–16 GPU-hour estimate likely reflects a small MLP, not a real SAE.

**Fix**: Scope the S1 deliverable to the prompt-side steering path only (which the synthesis already calls out as the "prompt-side half"). Explicitly defer the SAE path — including `sae_table_path` configuration, the `apply_steering` vtable method, and all on-device activation manipulation — to a future sprint, contingent on a feasibility study of SAE training on realistic persona bank sizes. The S1 `hu_persona_steering_directive` prompt generation is self-contained and delivers value without the SAE. Remove the "12–16 GPU-hours" claim from the risk register; replace it with an open research question.

---

### M5 — TTT DPO pair: "preferred" sample is synthesized, not observed — contrary to VDS-TTT methodology

**Initiative**: #05  
**Design doc ref**: `init-05-verifier-driven-ttt.md §4.1 "Happy path"`, References (VDS-TTT arXiv:2505.19475)

**Problem**: The VDS-TTT paper the design cites selects the `preferred` sample from the model's own high-scoring completions. Init-05's happy path does the opposite: when the user says "no, just bullets," the design synthesizes a preferred-label *from the correction text* ("synthesized hint from correction") rather than waiting to observe an actual good response. This is training on an LLM-generated pseudo-label as the ground truth of what "good" looks like — a circular dependency. If the synthesis is wrong (the model's paraphrase of "use bullets" is subtly off), the DPO update pushes the adapter toward the wrong target. The cited paper uses rollouts-then-select; init-05 uses synthesize-then-train. These are fundamentally different algorithms. The design should not cite VDS-TTT as validation for its approach.

**Fix**: Adopt the correct two-turn flow: (1) apply the user's correction as a forced next response (show the user a bullet-formatted version immediately), (2) record the user's reaction to the corrected response (implicit: no further complaint; explicit: thumbs-up signal), (3) THEN form the DPO pair using the accepted response as `preferred`. This turns the TTT loop into an observed-preference system rather than a synthesized-label system. If the immediate-correction response isn't practical (e.g., streaming has already committed), use the correction as a soft edit to the existing response rather than as a DPO training signal until an observed preferred sample is available.

---

### M6 — TTT adapter format contract between init-05 and init-04 is unspecified

**Initiative**: #05, #04  
**Design doc ref**: `init-05-verifier-driven-ttt.md §3.3 "Atomic adapter swap"`, `init-04-mlx-qwen3-provider.md §6.3 "load_adapter format validation"`

**Problem**: Init-05 calls `hu_provider_load_adapter` after a TTT step to hot-swap the updated adapter. Init-04's `mlx_qwen3_load_adapter` implementation returns `HU_ERR_PROVIDER_RESPONSE` with a "run `human ml lora-convert` first" message when given a `.lora` HUML-format file. Init-05's `snapshot_safe_adapter` writes the adapter to `~/.human/adapters/<contact-id>/active.bin`. The `.bin` extension is ambiguous — is this HUML binary or MLX safetensors? If it is HUML format (the natural output of `hu_learner_t`), `load_adapter` rejects it. If it is MLX safetensors (requires the subprocess to write it), it skips `lora_convert_provenance.json` tracking, which init-04 §6.3 uses to validate that a format conversion was performed. Either way, the two designs cannot simultaneously be correct without an explicit protocol agreement that neither document specifies.

**Fix**: Explicitly specify in init-05 §3.3: "The TTT learner backend (`mlx_qwen3_serve.py`) is responsible for writing the updated adapter in MLX safetensors format directly via the `snapshot_safe_adapter` opcode. A `provenance.json` sidecar MUST be written alongside the adapter file indicating TTT origin, bypassing the `lora-convert` check." Amend init-04's `load_adapter` to accept TTT-origin adapters (identified by the `provenance.json` field `origin: "ttt"`) without requiring the `lora-convert` path. Add a test: `mlx_qwen3_load_adapter_ttt_origin_accepted`.

---

### M7 — MoLoRA router `preferred_slot` boost is applied pre-softmax but semantics require post-softmax floor

**Initiative**: #02  
**Design doc ref**: `init-02-molora-channels.md §5.1 "Router implementation"`, `hu_molora_router_call`

**Problem**: The router code adds `boost = ctx->preferred_weight_floor` directly to the pre-softmax logit for the preferred slot: `y[preferred_slot] += boost`. However, `preferred_weight_floor` is documented as a *probability floor* (range [0.0, 1.0]) — "ensure the preferred expert is represented with at least this weight in the mixture." A probability floor must be applied *after* softmax (clamp the probability, then renormalize), not added as a logit. Adding 0.8 to a logit of -5.0 still produces a post-softmax probability far below 0.8. The design correctly handles `macro_mode_floor` with a post-softmax clamp (`y[0] = fmaxf(y[0], r->macro_mode_floor)` after softmax), but `preferred_slot` uses the logit-addition approach. These two floors use inconsistent implementations despite identical semantics.

**Fix**: Apply `preferred_weight_floor` identically to `macro_mode_floor`: call softmax first, then clamp the preferred slot probability, then renormalize. The logit addition can remain as an initialization hint if desired (it biases the distribution before softmax), but the actual floor guarantee must be enforced post-softmax. Add a unit test: `router_preferred_slot_floor_holds_after_softmax` that verifies `out_weights[preferred_slot] >= preferred_weight_floor` across a sweep of input logit values.

---

### M8 — Init-07 ThinkPRM binary delta is 10.1 KB with `HU_ENABLE_ML`, exceeding the 8 KB ceiling

**Initiative**: #07  
**Design doc ref**: `init-07-thinkprm-verifier.md §D6 "Binary budget"`

**Problem**: D6 states: "Default release: 7.8 KB delta." and "With `HU_ENABLE_ML` release total: ~10.1 KB." The codebase's release preset includes `HU_ENABLE_ML` as part of the standard ML feature set. The master coordinator's portfolio overview explicitly shows init-07 targeting users who run `HU_ENABLE_ML` (the PRM only runs on-device via the ML pipeline). The "7.8 KB" figure covers only the skeleton, not the shipped feature. The 10.1 KB figure violates the global 8 KB ceiling declared in D6 itself. This is a self-contradictory budget entry — the document both declares the ceiling and then exceeds it in the next sentence.

**Fix**: Either (1) reduce the implementation: the `cli_prm.c` CLI handler is the largest contributor (~1.8 KB) and can be stripped from the release binary by gating it behind `HU_ENABLE_DEVELOPER_CLI` instead of `HU_ENABLE_ML`, or (2) get an explicit budget exception approved at the portfolio level, citing that `HU_ENABLE_ML` users opt into the additional footprint. Either way, the D6 entry must not simultaneously claim an 8 KB ceiling and a 10.1 KB total — pick one or resolve the discrepancy.

---

### M9 — Init-14 cites `claude-opus-5-20260301` as a frontier model — this model ID is fabricated

**Initiative**: #14  
**Design doc ref**: `init-14-public-benchmarks.md §8 "Published report template"`, `hu_benchmark_compare_with_frontier`

**Problem**: The published report template and `hu_benchmark_compare_with_frontier` stub use `claude-opus-5-20260301` as the comparison model. This model does not exist. Per AGENTS.md §10.1: "Always verify model IDs before writing code. Model names change frequently." Hardcoding a fabricated model identifier into a public benchmark report template means every reproduction run will either (a) silently fail to call the comparison API, (b) return a "model not found" error that's swallowed, or (c) worse, be treated as valid comparison data with a null result. Benchmarks that cite a nonexistent model as their frontier baseline are not reproducible.

**Fix**: Replace `claude-opus-5-20260301` with the current known-good model identifier for the Claude family (verify via the Anthropic API before committing). Add a `FRONTIER_MODEL_VERSION` constant in `include/human/eval_benchmarks.h` that is set at build time from a configuration variable, with a CI check that validates the model ID resolves to a real API endpoint before the benchmark is run. This prevents the model-version problem from recurring every time Anthropic rotates model names.

---

## MINOR Findings

### N1 — Init-12 precondition requires two renames; synthesis only acknowledges one

**Initiative**: #12  
**Design doc ref**: `init-12-mcp-server-mode.md §1 "Preconditions"`, master coordinator synthesis §Preconditions

**Problem**: The synthesis precondition says "rename `hu_mcp_server_t → hu_mcp_client_t`" as a one-PR slice. The live codebase shows this is the type in `include/human/mcp.h` (the outbound MCP client connection type). But `include/human/mcp_server.h` separately defines `hu_mcp_host_t` — which init-12 intends to rename to `hu_mcp_engine_t` to make room for the new `hu_mcp_server_t` public vtable. This is a second non-trivial rename in a different file. If the precondition PR only targets `mcp.h`, the sprint implementer arrives at init-12 and discovers a second rename blocked behind a missed prerequisite.

**Fix**: Expand the precondition PR description in the master coordinator to explicitly cover BOTH renames: (1) `include/human/mcp.h:hu_mcp_server_t → hu_mcp_client_t` and (2) `include/human/mcp_server.h:hu_mcp_host_t → hu_mcp_engine_t`. They can land in the same PR since both are mechanical renames with no behavior change. Add a grep-based CI check that the old names do not appear outside the respective headers post-rename.

---

### N2 — `HU_TRUST_*` prefix collides with existing constants in two unrelated headers

**Initiative**: #09  
**Design doc ref**: `init-09-memory-trust-tiers.md §2.1 "hu_trust_tier_t"`

**Problem**: The live codebase already defines `HU_TRUST_*`-prefixed constants in two headers: `include/human/intelligence/trust.h` (quality-scoring constants: `HU_TRUST_ACCURATE_RECALL`, `HU_TRUST_CORRECTION`, `HU_TRUST_FABRICATION`) and `include/human/behavior/trust.h` (response-behavior constants: `HU_TRUST_ANSWER`, `HU_TRUST_DISCLOSE_UNCERTAINTY`, `HU_TRUST_REFUSE_TO_AGREE`). Init-09's proposed `HU_TRUST_USER_DIRECT`, `HU_TRUST_PERSONA_DERIVED`, `HU_TRUST_UNTRUSTED` add a third `HU_TRUST_*` family with a completely different semantic domain (memory provenance tiers). While there is no compile-time collision (different enum types), any developer grepping for `HU_TRUST_` to understand the trust model will hit all three systems simultaneously.

**Fix**: Name-scope the memory provenance enum: use `HU_MEM_TRUST_USER_DIRECT`, `HU_MEM_TRUST_PERSONA_DERIVED`, etc., or `HU_PROV_TIER_USER_DIRECT`. This clearly scopes the constants to the provenance subsystem and eliminates namespace ambiguity. Update the design doc accordingly.

---

### N3 — Quarantine log uses `%s` for user-controlled content without JSON escaping

**Initiative**: #09  
**Design doc ref**: `init-09-memory-trust-tiers.md §2.7 "Quarantine log schema"`

**Problem**: The quarantine log uses `snprintf(line, sizeof(line), "...\"channel\":\"%s\",\"handle\":\"%s\"...", prov->channel, prov->contact_handle, ...)`. Both `prov->channel` (max 64 bytes) and `prov->contact_handle` (max 128 bytes) are user-derived strings from inbound message metadata. Neither field is JSON-escaped before interpolation. A channel name or handle containing `"`, `\n`, or `\` will produce malformed JSON in the quarantine log. If audit tools parse this log with a strict JSON parser, the log entry for an injection attempt will itself fail to parse — precisely the scenario where the quarantine record matters most.

**Fix**: Either (a) use a minimal JSON string escaper before interpolation, or (b) write the quarantine log in a simpler format (TSV or newline-delimited bare fields) that requires no escaping. A TSV quarantine log is more robust and easier to `grep` than JSON. Update the schema example and any consumers (audit CLI).

---

### N4 — Init-11 dispatcher integration is ambiguous about which file to modify

**Initiative**: #11  
**Design doc ref**: `init-11-proactivity-typing.md §D2 "Modification B"`

**Problem**: §D2 says: "Replace direct `ch->vtable->send(...)` for proactive paths with `hu_typing_send(..., &profile)` in `dispatcher.c` OR `channels/dispatch.c`." The "OR" indicates the architect is uncertain which file owns the proactive send path. The `src/agent/CLAUDE.md` module map distinguishes `dispatcher.c` (routes incoming messages) from the proactive send in `awareness.c → proactive.c`. A grep-led investigation is needed to confirm which file actually calls `ch->vtable->send` on outgoing proactive messages. If it is `awareness.c` (the contextual awareness layer), neither `dispatcher.c` nor `channels/dispatch.c` is the right target.

**Fix**: Before sprint kickoff, run `grep -rn "vtable->send" src/agent/ src/channels/` to find the actual outgoing proactive send call sites. Update §D2 to name the exact file and function. This ambiguity, if unresolved at implementation time, will cause the typing simulation to be wired to the wrong path — either not firing at all or double-firing on channels that use a different dispatch route.

---

### N5 — `macro_mode_floor=0.0` silently breaks the "macro adapter is always present" invariant

**Initiative**: #02  
**Design doc ref**: `init-02-molora-channels.md §5.1`, `hu_molora_config_t` definition

**Problem**: The design states "slot 0 (macro mode) is always included in the mixture." But the invariant is implemented as a post-softmax floor: `y[0] = fmaxf(y[0], r->macro_mode_floor)`. If `macro_mode_floor = 0.0` (which is a legal config value), `fmaxf(y[0], 0.0)` is a no-op — slot 0 participates only if it already ranks in the top-3. The config struct allows setting `macro_mode_floor = 0.0`. An operator who sets this thinking they're "disabling the floor override" will actually break the persona-continuity guarantee without any warning.

**Fix**: Either (a) clamp `macro_mode_floor` to a minimum of `1.0 / HU_MOLORA_MAX_SLOTS` (≈0.125) at config validation time with a logged warning, or (b) document explicitly that `macro_mode_floor=0.0` disables the invariant and name the field `macro_mode_min_weight` to make the zero-value behavior intuitive. Add a test: `router_macro_mode_floor_zero_does_not_guarantee_slot0`.

---

### N6 — W7 episode-type cleanup is labelled "optional in S1" but is mandatory before S2

**Initiative**: #10  
**Design doc ref**: `sota-2026-massive-team-program.md` synthesis §Preconditions, `init-10-episode-storage-sleep-consolidation.md §11`

**Problem**: The master coordinator says "W7 rename `hu_episode_t` collisions (~1 day, optional in S1)". Init-10 §11 says "W7 lands renames first (no behavior change)" as the first step in its migration sequence. As established in B4, there is already an ODR violation with two live `hu_episode_t` structs in the codebase. "Optional in S1" is an operational fiction: if W7 slips out of S1 and into S2, init-10 cannot start in S2 because its own migration sequence requires W7 to be complete as step 1. The label "optional in S1" should be replaced with "required pre-S2, schedule in S1".

**Fix**: Update the master coordinator synthesis to: "W7 rename (~1 day): schedule as first W of S2 or last W of S1. Required for init-10 to proceed. Non-optional."

---

## NIT Findings

### T1 — P0 line numbers are stale in both design docs

Init-09 §3.1 says `agent_stream.c:345`. Master coordinator says `agent_stream.c:370`. The actual call is at line 370 (confirmed by `Read`). More importantly, there is a **second** `hu_personal_model_ingest` call in `agent_stream.c` (around line 2460 in the file as currently structured) that neither document acknowledges. Both docs should be corrected, and the second call site should be assessed for provenance.

### T2 — "No public surface additions" claim in init-01 is incorrect

Init-01's executive summary says the initiative is "strictly additive to existing APIs with no public surface deletions." But `include/human/persona/steering.h` is a brand-new public header with 5+ new types and 2 new public functions. This IS a public surface addition. The claim is probably intended to mean "no deletions" or "no vtable breaking changes," but the precise wording is wrong and could mislead a binary-compatibility audit.

### T3 — `eval-reproduce.sh` reproducibility claim is unverifiable without pinned dataset hashes

Init-14 §D3 produces `scripts/eval-reproduce.sh` and claims `sha256sum --check .eval-checksums` verifies data integrity. But the dataset pull commands in the script use URLs without commit hashes or content-addressed refs (e.g., for KnowU-Bench). Dataset maintainers can update the dataset without changing the URL. Add `--depth=1 --ref=<git-sha>` pins or content-address the data files in `tests/eval/<suite>/data/` to make the reproducibility guarantee meaningful.

---

## S1 Verdict

### #09 — Memory Trust Tiers — GO-WITH-FIX

**Conditions that must be met before this initiative merges:**
1. Fix B2 (migration default changed from FIRST_PARTY to THIRD_PARTY or UNTRUSTED for pre-existing rows).
2. Fix B3 (full call-site sweep of `hu_personal_model_ingest`; patch all callers).
3. Fix B4 prereq: W7 does not need to be complete for #09, but #09 must not introduce a new `hu_episode_t` dependency; confirm the header graph.
4. Address N2 (rename `HU_TRUST_*` to `HU_MEM_TRUST_*` or equivalent before the header is public).
5. Document the MINJA detector's scope limitations accurately in the risk register (M1).

The trust-tier enforcement logic itself is sound. The migration inversion (B2) and call-site incompleteness (B3) are fixable in a day each. The security value of even an imperfect trust tier is positive — ship it with the caveats corrected.

---

### #04 — MLX Qwen3 Provider — GO-WITH-FIX

**Conditions that must be met before this initiative merges:**
1. Fix M2 (pipe-polling dead-process detection; do not rely solely on 30s timeout).
2. Fix M3 (decouple keepalive watchdog from per-request generation budget).
3. Clarify M6 in the design doc: document the adapter format that `snapshot_safe_adapter` produces, even as a forward reference to init-05.

The core provider architecture (persistent Python helper, length-prefixed JSON protocol, W13-compliant atomic adapter write, opcode reservations for #02 and #05) is solid. The timeout behavior is a reliability bug, not a correctness bug; the feature delivers meaningful value and the fixes are contained within `mlx_qwen3.c` and the test suite.

---

### #14 — Public Benchmarks — GO-WITH-FIX

**Conditions that must be met before this initiative merges:**
1. Fix M9 (replace `claude-opus-5-20260301` with a verified model ID; add `FRONTIER_MODEL_VERSION` guard).
2. Fix T3 (pin dataset hashes in `eval-reproduce.sh`).

The benchmark architecture (reuse of `hu_eval_suite_t`, five new suite directories, `results.schema.json`, CLI extensions) is additive and non-breaking. The fabricated model ID is the only blocker — it would make every published benchmark report cite a nonexistent model.

---

### #01 — Activation Steering (prompt-side half) — GO-WITH-FIX

**Conditions that must be met before this initiative merges:**
1. Fix M4 in the design doc: explicitly defer the SAE path (`sae_table_path`, `apply_steering` vtable, on-device activation manipulation) to a future sprint. The S1 scope should be: `hu_persona_steering_directive` prompt string generation only.
2. Fix T2 in the design doc: correct the "no public surface additions" claim — `steering.h` IS a new public header.

The prompt-side steering path (persona → vector projection → directive string) is entirely self-contained and valuable. The SAE path is currently vaporware with an over-optimistic compute budget estimate. Separating them in the design doc prevents scope creep during implementation.

---

### #11-typing — Typing Simulator — GO-WITH-FIX

**Conditions that must be met before this initiative merges:**
1. Fix N4 (resolve the dispatcher file ambiguity before implementation; confirm the exact call site via grep).
2. Confirm that `hu_typing_send` under `HU_IS_TEST` is a no-op (design says it is; add a test that asserts zero actual sleep time in test mode).
3. Add a test for the 3-defer-then-SUPPRESS behavior that validates the user-visible consequence (a suppressed message that would have been useful should surface in the inbox).

The xorshift64-based typing simulator and the logistic PRISM gate are well-designed. The 3-defer cap producing silent suppression (M1-adjacent: see N5 in the MINOR section re: `bounded re-defers`) should add user-facing suppression telemetry before shipping.

---

## Things the Synthesis Got Right (Protect These)

**1. The `hu_provider_t.load_adapter` ship-order contract (04 → 02 → 05)** is correct and load-bearing. Slot reservations in the Python protocol (`load_adapter_mix`, `mutate_adapter`) prevent breaking changes as later initiatives land. Do not let any implementer "simplify" init-04 by removing the reserved opcode stubs.

**2. Blocking proactive notifications when `encrypt_at_rest=true` and keystore is locked** (init-10 vs init-11 conflict resolution) is the right security call. The alternative — reading partially decrypted or empty episode summaries to decide whether to send a proactive message — would produce both false-positives (spammy) and information leakage. Protect this.

**3. Init-02 owning `hu_persona_overlay_t.typing_profile`** with init-11 as a read-only consumer is the correct coupling direction. Reversing this (init-11 owning a field in a struct that init-02 reads) would create a dependency inversion that makes the MoLoRA router depend on the typing subsystem at compile time.

**4. The `lora-convert` provenance tracking** (init-04 §6.3) — requiring a `provenance.json` sidecar before loading an adapter — is good defensive programming. It prevents accidental loading of HUML-format adapters in an MLX context. The TTT integration (M6) needs to extend this contract, not bypass it.

**5. The W13 atomic adapter write pattern** (`tmp + fwrite + fflush + fsync + rename`) for all adapter file mutations. The personal-model atomic save is already pinned by a deterministic adversary test (`test_personal_model_save_preserves_prior_state_when_tmp_blocked`). Apply the same test pattern to every new atomic write in init-04, init-05, and init-10.

**6. `hu_reward_model_t` as the single shared surface between init-05 and init-06** prevents duplicated PRM implementations. Do not let TTT (init-05) absorb its own inline quality scorer — keep `hu_reward_model_t` as the canonical oracle.

---

## Most Important Finding

**The single finding that must be fixed before any S1 code ships is B2 — the migration default.**

Init-09 is the security spine of SOTA-2026. Every downstream initiative (#04, #05, #10, #11, #12) is predicated on the trust tier system being correct. The migration grants FIRST_PARTY trust (tier 2 of 4) to every row in the existing memories database — including any facts that were injected by the very MINJA/MemoryGraft attacks the initiative is designed to block. An attacker who poisoned a user's memory store before the migration ships will have their injected facts elevated to first-party trust, making them harder to overwrite and less likely to be quarantined by the new gate logic. This is not a latent risk or an edge case: it is a migration that, as written, makes the pre-existing poisoned-memory threat *worse* for every user who had poisoned memories before the upgrade. Fix the migration default to THIRD_PARTY (1) or UNTRUSTED (0) before writing a single line of sprint S1 code.

---

```
RESULT_critic=HAS_FINDINGS_4_9
```

_4 BLOCKERs, 9 MAJORs, 6 MINORs, 3 NITs_
