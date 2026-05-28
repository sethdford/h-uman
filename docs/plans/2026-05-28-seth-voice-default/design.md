# Seth-Voice Default — Design

Policy decisions from requirements sign-off (2026-05-28): **all three phases**;
**local-first whenever a healthy adapter+server exist** (no fidelity pre-gate),
with mandatory cloud fallback as the safety net.

## Verified starting state (file:line)

| Fact | Location | Implication |
|---|---|---|
| Override-to-local gate requires explicit opt-in | `src/agent/model_router.c:226` `maybe_override_to_mlx_local` — `if (!cfg->mlx_local_enabled \|\| !cfg->mlx_local_healthy \|\| !model) return;` | AC-1: relax gate so healthy+adapter is sufficient by default |
| Router default conv model is **deprecated** | `src/agent/model_router.c:136` `"gemini-3-flash-preview"` | Out-of-scope bug (separate chip); CLAUDE.md says use `gemini-3.5-flash` |
| Example selection ignores style | `src/persona/examples.c:228` scores by `keyword_overlap` only | AC-7: add style-affinity scoring |
| Personal-model block injected separately | `src/agent/agent_turn.c:3577`, `src/agent/agent_stream.c:1078` | AC-7 reads the same fingerprint the block already uses |
| Adapter swap curl-gated | `src/ml/mlx_admin.c:194` returns `HU_ERR_NOT_SUPPORTED` when `!HU_ENABLE_CURL` | AC-6: clear-log no-op in dev; works in release (curl ON) |
| Existing turn fallback hook | `src/agent/agent_stream.c:230` `fallback_err` path | AC-2: extend for local→cloud |
| DPO source tagging supported | `src/ml/dpo_miner.c:340-355` `pair.source` (e.g. `outbound_edit`) | AC-5: tapback path writes a real source tag |
| `reaction_collection` already whitelisted | `src/config_validate.c:68` (since 2026-05-18) | AC-4: warning already gone; only default flip remains |
| Nightly train→swap wired | `src/ml/lora_nightly.c:46` `should_run(threshold)`, `:202` swap call | AC-6: mostly verify + curl-off log |
| Doctor check module pattern | `src/doctor/check_chatdb.c`, `check_outbound_stats.c` | AC-3: add `check_local_voice.c` |

## Components

### Phase 1 — Default local path + closed loop
- **Local-first routing policy** — `src/agent/model_router.c`. New tri-state `cfg->mlx_local_routing` enum `{OFF, AUTO, FORCE}` (or reuse `mlx_local_enabled` + a new `mlx_local_auto` bool). In `AUTO` (the new default), `maybe_override_to_mlx_local` fires when `mlx_local_healthy && mlx_local_model` regardless of the explicit opt-in. `OFF` preserves cloud-only for users who set it. Keeps the router pure — health is computed by the caller.
- **Local health probe** — small helper (likely `src/agent/model_router_health.c` or in `from_config.c`) that sets `mlx_local_healthy` by checking adapter-file presence (path + nonzero size) and a cached MLX server reachability ping (TTL ~60s, reuses `hu_http_post_json` HEAD/health). Never blocks the turn; stale-but-cached is acceptable.
- **Local→cloud turn fallback** — `src/agent/agent_stream.c` (extend the `:230` fallback path) + `agent_turn.c`. On local provider error, timeout, or response_guard-degenerate, retry once on the cloud provider in the same turn; emit one `hu_log_*` line naming the reason. Reuses the existing fallback plumbing, not a new path.
- **Local-voice doctor check** — `src/doctor/check_local_voice.c` reporting three sub-states: adapter present (path/sha/size), MLX server reachable, curl-enabled build. Registered alongside existing checks.
- **reaction_collection default-on** — `src/config_merge.c` seeds `cfg->reaction_collection.enabled = true`. (No validator change needed — already whitelisted.)
- **Real DPO pair sourcing** — verify/wire the iMessage tapback + user-edit collectors write `pair.source = "imessage_tapback"` / `"user_edit"` through `dpo_miner`. Add the source constants if missing.
- **Nightly train→swap closure** — `src/ml/lora_nightly.c`: confirm `should_run` threshold reads real-pair count; ensure curl-off path logs a clear "swap skipped: build lacks curl" once rather than silent `HU_ERR_NOT_SUPPORTED`.

### Phase 2 — Fidelity quality
- **Style-affinity example selector** — `src/persona/examples.c`. New scoring that combines topic overlap with distance from the personal-model fingerprint (`lowercase_ratio`, `abbreviation_ratio`, avg length). Signature takes an optional `const hu_communication_style_t *` (NULL → today's behavior exactly, AC-8). A per-example style is computed cheaply from the stored `response` text (case ratio, abbreviation hits, length).
- **Learned length guard** — `src/agent/response_guard.c`. G5 baseline becomes `contact/channel`-derived (personal_model.style.avg_message_length + contact relationship stage) instead of the universal relative-multiplier-with-floor. Keep the absolute floor as a backstop when no baseline exists.
- **Per-contact shape caps** — `include/human/eval/shape.h` + `src/.../shape.c`. The length cap accepts an optional learned bound; close contacts may exceed the universal cap up to that bound. Structural fails (markdown, AI openers) unchanged.

### Phase 3 — Measurement
- **Live-path fidelity eval** — extend `scripts/eval_fidelity_nightly.py` (or a sibling) to invoke the actual router-chosen path and record `path_used` + score in the verdict JSON.
- **Blind human-eval harness** — new `scripts/blind_eval_export.py` + `scripts/blind_eval_ingest.py`: export anonymized real-vs-model pairs to a rateable file; ingest 1–10 ratings on the fixed rubric (tone, vocabulary, humor, decision-style); emit per-dimension aggregate.

## Data flow (Phase 1 turn)
1. Turn arrives → `model_router` computes tier + selection.
2. Health helper sets `mlx_local_healthy` (cached). In `AUTO`, `maybe_override_to_mlx_local` selects local Gemma+LoRA for REFLEXIVE/CONVERSATIONAL.
3. Provider call runs (buffered; streaming stays off per non-goals).
4. On success → outbound guard chain → reply. On failure/degenerate → cloud fallback (one retry, logged once) → guard chain → reply. **A reply is always produced.**
5. Post-turn: reaction_collection observes tapbacks/edits → `dpo_miner` writes real-sourced pairs.
6. Nightly: if real-pair count ≥ threshold and `nightly_lora_enabled`, train → `hu_mlx_admin_swap_adapter` hot-loads the new adapter (release) or logs-skip (dev).

## Decisions
- **Tri-state routing with `AUTO` default (not flipping `mlx_local_enabled` to true)** — serves AC-1, AC-2. A bare boolean flip would force local even when unhealthy and surprise cloud-only users; `AUTO` = "local when healthy, else cloud" encodes the local-first-with-fallback policy explicitly and keeps `OFF` available.
- **Health computed by caller, router stays pure** — serves AC-1, AC-3. Matches the existing comment at model_router.c:219 ("router stays pure: mlx_local_healthy is set by the caller"). Testable without a live server.
- **Reuse existing fallback plumbing at agent_stream.c:230** — serves AC-2. Avoids a parallel code path; the degenerate-output fallback already exists, we add the provider-failure trigger.
- **Optional-style-param selector, NULL = identical behavior** — serves AC-7, AC-8. Guarantees no regression for cloud-only/no-model users and makes the no-op case byte-pinned.
- **Keep absolute floor as backstop in G5** — serves AC-9. The terseness-death-spiral fix (commit ab6d000c) stays as the safety net when no learned baseline exists.
- **reaction_collection default-on, nightly_lora stays opt-in** — serves AC-4; honors the non-goal. Collecting feedback is safe and cheap; training/swapping is heavier and operator-gated.
- **Live eval invokes the real router path** — serves AC-11. Offline-only eval is what made the +27pp number untrustworthy for production; measuring the chosen path is the whole point.

## Risks
- **Bad adapter served by default (local-first, no pre-gate)** — mitigated by AC-2 fallback (degenerate output → cloud) + AC-9/AC-10 guards + the existing `lora-scale-default-or-die.md` discipline. If a future adapter regresses, the guard+fallback catch it within one turn; AC-11 live eval surfaces it.
- **Health probe adds per-turn latency** — mitigated by ~60s TTL cache; first-miss cost is one localhost ping, amortized.
- **Dev builds can't swap (curl OFF)** — accepted; AC-6 makes it a clear logged no-op. Release (curl ON) is the real path.
- **Stale agent claims in the audit** (e.g. "warning unfixed") — mitigated by the verified starting-state table above; every Phase-1 task starts from a re-confirmed file:line, per `audit-verify-before-allege.md`.
- **`gemini-3-flash-preview` deprecated default** (model_router.c:136) — out of scope here; tracked separately so this spec stays single-concern.
- **Test/source gate symmetry** — any new flag-gated source (`check_local_voice.c`, style selector under a flag) must follow `test-source-gate-symmetry.md` or CI variants break.
