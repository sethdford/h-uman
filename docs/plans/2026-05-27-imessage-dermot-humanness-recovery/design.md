# iMessage Dermot Humanness Recovery — Design

**Status:** DRAFT — pending approval before tasks.md.
**Builds on:** requirements.md in this directory.

## Codebase reconnaissance (verify-before-allege per `~/.claude/rules/audit-verify-before-allege.md`)

Before writing this design I confirmed the following in the live tree:

| Claim | Evidence |
|---|---|
| Self-RAG substitution happens at agent-level, not just bridge-level | `src/agent/agent.c:1609-1617` — `hu_agent_self_rag_apply` propagates `modified` via `*swapped_out` when can_swap is true. All four substitution sites (`world_model_bridge.c:752`, `self_rag_inline.c:439,523`, `self_rag_atomic.c:715`) feed into this one consumer. |
| Threading dispatcher already plumbs `parent_msg_guid` through | `src/daemon.c:1023-1026` — `ch->vtable->reply(ch->ctx, target, target_len, parent_msg_guid, parent_guid_len, body, body_len)`. The GUID is already there; the only gap is the Tier 1/2 stubs return false. |
| `action_surface_v2.enabled` is TRUE in live config | `~/.human/config.json:95`. The outer dispatcher gate is open; nothing else upstream is blocking threaded send. |
| Cmd-R / AX Show Menu helpers (`ax_open_conversation`, `ax_find_message_group`) already exist | `src/channels/imessage.c:797,3287,3459`. The Tier 1/2 stubs are intended to call exactly these helpers — the comments in `imessage_reply.c:54-72` even reference them by name. |
| MLX local provider exists but no `health_check` symbol in `src/providers/mlx.c` | Direct grep returned 0 matches for `health_check`/`/health`/`server_health`. Component 2 must ADD a health probe. |
| `hu_model_route` takes `cfg->conversational_model` as a string and returns it verbatim in `hu_model_selection_t.model` | `src/agent/model_router.c:230-231`. The router is purely string-based; provider lookup happens downstream by name. |
| `hu_hallucination_guard` is a SEPARATE guard from self-RAG | `src/agent/agent_turn.c:7679-7700`. It rewrites *only when memory contradicts* a claim. This stays on after Component 1; the over-refusal we're removing was firing on "no memory matches" which is a different signal. |

Three things I expected to find but didn't, which de-risk the design:

1. **No undiscovered "policy refusal" path triggered by ABSTAINED.** AC-3's policy refusal at `agent_stream.c:2034` is gated by `HU_REFUSAL_POLICY` (not `HU_REFUSAL_UNKNOWN_FACT`), so changing ABSTAINED behavior doesn't touch it.
2. **No second consumer of `*swapped_out` outside `agent_turn.c`.** A single grep for `hu_agent_self_rag_apply` confirms the consumer fan-out is bounded.
3. **No alternate AX backend** for threaded reply (e.g. JXA AppleScript). The `imessage.c` CLAUDE.md doc note explicitly says "no AppleScript verb for threaded reply-to-specific-message" — confirms AX is the only path.

## Components

### C1 — Self-RAG abstain pass-through (with D6 backend cleanup)

This component has two layers: a defense-in-depth consumer guard AND
backend-level cleanup so the wasted work doesn't happen in the first place.

**Crucial distinction surfaced during recon:** `HU_W11_OUTCOME_ABSTAINED` is
emitted by TWO different paths inside the backends:

- **Policy refusal** — the LLM itself (in inline-mode) emitted a
  `<refuse>…</refuse>` tag. The refusal template here is `HU_REFUSAL_POLICY`.
  Per AC-3, this MUST keep substituting (it's the model self-redacting; the
  original draft contains the unredacted thing we should not send).
- **Score-based abstention** — verifier found low confidence /
  unknown-fact / negative-memory-match. Template is `HU_REFUSAL_UNKNOWN_FACT`
  or `HU_REFUSAL_LOW_CONFIDENCE` or `HU_REFUSAL_NEGATIVE_MEMORY_MATCH`. This
  is the one we want to PASS THROUGH.

The two paths must be disambiguated at the BACKEND level (the backends know
which template they rendered), not at the consumer (which only sees the
final outcome enum and refusal_text buffer).

**Changes:**

1. **`src/agent/self_rag_inline.c:508-529`** — the STRICT-mode score-based
   abstention block. Remove the two lines that copy `resp->refusal_text` →
   `resp->modified_draft` and that set `resp->draft_modified = true`. Keep
   the `outcome = HU_SELF_RAG_ABSTAINED` set and the `refusal_text` render
   (the latter feeds telemetry / log lines, not the draft swap). The
   POLICY refusal at line 486-494 is **untouched** (AC-3).

2. **`src/agent/self_rag_atomic.c:715`** — analogous score-based abstention.
   Same treatment: stop writing `modified_draft`, keep `outcome` and
   `refusal_text` render.

3. **`src/agent/world_model_bridge.c:746-764`** — the heuristic-backend
   dispatcher. Today it unconditionally renders `HU_REFUSAL_UNKNOWN_FACT`
   when `resp.outcome == ABSTAINED && resp.refusal_text[0] == '\0'`. Split
   the branch: if `resp.refusal_text[0] != '\0'` (came from a backend
   that DID render — that's the policy path now), keep propagating. If
   `refusal_text[0] == '\0'` (heuristic backend's own score-based abstain),
   set `src = NULL; src_len = 0;` so `*out_modified` stays NULL.

4. **`src/agent/agent.c:1609-1617`** — defense-in-depth at the consumer.
   After the backend changes, this block already won't see a swap on
   score-based ABSTAINED (because `modified` will be NULL). Add an
   explicit guard: if `outcome == HU_W11_OUTCOME_ABSTAINED && modified`,
   free `modified` and leave `*swapped_out = NULL`. This catches any
   future backend that forgets to honor the new contract.

**What stays:**

- The `agent->self_rag_abstentions++` counter still increments (AC-4).
- The `agent->self_rag_refusals_rendered` counter still increments **only
  when a refusal was actually rendered into the outbound** — which after
  this change is only the policy-refusal path. The natural divergence
  between the two counters becomes a useful operator signal.
- HEDGED, REWRITTEN, and any other modified-draft outcomes propagate
  unchanged (AC-2).
- Policy refusal at `self_rag_inline.c:486-494` is untouched (AC-3).

### C2 — Model router routes Seth-voice to mlx_local

Three changes:

1. **Add `hu_mlx_local_probe` to `src/providers/mlx.c`** — `bool hu_mlx_local_probe(void)` that does a non-blocking HTTP GET to `http://127.0.0.1:<port>/health` (port from config) with a 300 ms timeout. Returns true iff response is 200. Cache result for 60 s to avoid hammering on every turn.
2. **Extend `hu_model_router_config_t`** in `include/human/agent/model_router.h` with `bool mlx_local_enabled; const char *mlx_local_model; size_t mlx_local_model_len;`. When `mlx_local_enabled == true` AND `hu_mlx_local_probe()` returns true, `hu_model_route` emits `mlx_local_model` for `tier == conversational` and `tier == reflexive`. Falls back to `conversational_model` / `reflexive_model` (the Gemini ones) otherwise.
3. **Edit `~/.human/config.json`** model_router block to set `mlx_local_enabled: true` and `mlx_local_model: "seth-lora-v4-repair-20260525-071921"` (a name the mlx provider knows). The Gemini tier defaults stay as the fallback.

### C3 — Real iMessage threaded reply AX wiring

Two stubs become real implementations in `src/channels/imessage_reply.c`:

- **`ax_reply_tier1_cmd_r`**: open conversation → find parent message group → focus it → synthesize Cmd-R → poll for composer field → type body → synthesize Return. The Cmd-R step uses `CGEventCreateKeyboardEvent` already used elsewhere in `imessage.c`. The "find parent message group" step uses `ax_find_message_group(window, content_prefix, 0)` where `content_prefix` is the first 32 chars of the parent body (looked up via the parent_msg_guid → text mapping in chat.db).
- **`ax_reply_tier2_show_menu`**: open conversation → find parent message group → `AXUIElementPerformAction(msg_group, kAXShowMenuAction)` → iterate the context menu items → match `title.startswith("Reply")` (handles "Reply…" with U+2026 and "Reply..." with 3 dots, plus localized variants) → press it → poll for the inline composer → type body → Return.

Both functions are `#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)`-gated (the same compile guard already in `imessage_reply.c:51`). Under `HU_IS_TEST` they return false immediately so the test-stub mechanism continues to drive Tier 3.

**New helper:** `parent_guid_to_text_prefix(guid, out_prefix, prefix_cap)` looks up the parent message text via chat.db read (read-only sqlite open already established elsewhere in the codebase). Cap the prefix at 32 chars to match `ax_find_message_group`'s heuristic match. This helper lives in `src/channels/imessage_reply.c` as a file-local `static`.

## Data flow (after all three components)

```
1. iMessage poll picks up Dermot's "Going out with a bang?!"
2. daemon.c::batch_process classifies action=0 (reply)
3. hu_model_route returns
     {model: "seth-lora-v4-repair-...", tier: conversational, source: heuristic}
   because mlx_local_enabled=true AND probe returned 200.   ── [C2]
4. agent_turn.c calls hu_provider::generate → mlx-server →
     LoRA-shaped draft "haha out with style? you doing one last
     dinner with the leadership crew or just dipping?"
5. hu_agent_self_rag_apply runs verifier:
     claims extracted = 0 (banter, no factual claims)
     outcome = HU_W11_OUTCOME_ABSTAINED
     counters incremented (telemetry preserved)
     *swapped_out = NULL  (pass-through)                     ── [C1]
6. agent_turn keeps original draft
7. hu_hallucination_guard runs separately — nothing to hedge (no claims)
8. daemon dispatch picks HU_REPLY_STYLE_THREADED
9. ch->vtable->reply == hu_imessage_reply with parent_msg_guid
10. ax_reply_tier1_cmd_r:
      open_conversation("+447914633409")
      find_message_group(window, "Going out with a ban", 0)
      focus + Cmd-R
      type body via CGEventKeyboardSetUnicodeString
      Return                                                  ── [C3]
11. tier_used = "cmdR" — telemetry emits via imessage_action.jsonl
12. Dermot sees a Seth-voiced reply attached as a native thread reply.
```

## Decisions (each tied to ≥1 AC)

| # | Decision | Rationale | ACs |
|---|---|---|---|
| **D1** | Belt-and-suspenders: backend-level cleanup at the 3 score-based-abstain sites (inline, atomic, world_model_bridge dispatcher) PLUS a consumer-level guard in `hu_agent_self_rag_apply`. Policy refusal path (one site in `self_rag_inline.c:486-494`) is untouched. | Backends know which template they rendered; they can correctly distinguish policy from score-based. Consumer guard catches any future backend that forgets the contract. | AC-1, AC-3, AC-4 |
| **D2** | Keep HEDGED/REWRITTEN substitution untouched. | Those outcomes mean "the verifier found contradicting evidence" — we DO want to swap in the hedged draft. Only ABSTAINED ("no evidence either way") is the over-fire case. | AC-2 |
| **D3** | Add an explicit `mlx_local_enabled` config flag rather than reusing `primary_provider`. | `primary_provider` is a separate concept (default provider factory); conflating routing with provider-factory selection causes confusion. Explicit flag is auditable. | AC-5, AC-6 |
| **D4** | Probe mlx-server health via HTTP /health with 60s cache, NOT via Unix socket / direct connect. | mlx-server already speaks HTTP; reusing the same surface keeps the integration loose. 60s cache amortizes the probe across many turns without serving stale data on real outages. | AC-8 |
| **D5** | Fall back silently (single WARN, then quiet) when mlx-server is down. NOT a hard fail. | The user must keep getting replies even when local serving is down. Spamming logs would drown signal. | AC-8, R3 |
| **D6** | Analytical/Deep tiers stay on Gemini. | Local 31B is good at chat but not at hard reasoning. Per `~/.claude/rules/lora-scale-default-or-die.md` even validated LoRAs trade off base capability for voice. Cloud is the right backstop for analysis. | AC-7 |
| **D7** | Implement BOTH Tier 1 (Cmd-R) and Tier 2 (Show Menu) — not just one. | Cmd-R works when the parent is visible and focusable. Show Menu works in more states (when Cmd-R doesn't focus the right row). Belt+suspenders for AC-9's 95% target. | AC-9 |
| **D8** | Use a 32-char text-prefix lookup via chat.db sqlite (read-only) to find the AX target. | `ax_find_message_group` already accepts a content_prefix parameter. The parent_msg_guid → text lookup is a single SQL read; cheap and reliable. | AC-9 |
| **D9** | AC-10's "WARN once per process" uses a static `bool warned_ax_unavailable = false;` guard, NOT per-call. | Spamming logs every reply attempt is worse than the original bug. One-shot is the discoverability discipline from `~/.claude/rules/silent-config-gated-subsystems.md`. | AC-10 |
| **D10** | Live-macOS-only ACs (AC-9, AC-12, AC-13) get a SCRIPTED manual recipe checked into the spec dir, not a CI test. | These cannot be CI'd. A reproducible recipe (`scripts/verify_dermot_recovery.sh`) lowers manual cost; honesty about the test gap is better than fake CI coverage. | AC-9, AC-12, AC-13 |

## Telemetry & observability

After Component 1, `agent->self_rag_abstentions` keeps counting, but `agent->self_rag_refusals_rendered` STOPS incrementing (because we no longer render). That divergence between the two counters is itself a useful operator signal: "abstain count vs. refusal-render count" goes from 1:1 to N:0 after C1 ships. Document this in the doctor health-check.

After Component 2, the `model route:` log line at `daemon.c:10511` will start showing `seth-lora-v4-repair-...` for conversational turns. `grep "model route:" ~/.human/logs/service-loop-error.log | awk '{print $3}' | sort | uniq -c` becomes a fast smoke test.

After Component 3, `imessage_action.jsonl`'s `tier_used` field will start showing `cmdR` or `ax_menu` for THREADED-style entries instead of 100% `flat_fallback`. Operator dashboard counter.

## Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | AX wiring fails on common states (screen locked, conversation not visible, focus stolen by other app). | High | Reply degrades to flat send. | Tier 3 (`flat_fallback`) is already the existing safety net. One-shot WARN at AC-10 makes the degradation visible to operator. |
| **R2** | LoRA local serve is slower than Gemini cloud → user-perceptible reply latency. | Medium | Replies feel sluggish. | `hu_mlx_local_probe` returns false on /health timeout (300ms cap). Per-turn timeout in the provider layer (existing) falls back to Gemini if local can't serve in budget. |
| **R3** | Pass-through lets a confidently-wrong factual claim slip through. | Low | Embarrassing reply asserting something untrue. | `hu_hallucination_guard` (separate from self-RAG) still runs and rewrites when memory CONTRADICTS a claim. The ABSTAINED case we're removing was "no memory matches" which is different — banter doesn't have memory to match, AND has no claim to contradict. |
| **R4** | Test gap on AC-9/12/13 means a regression could ship undetected. | Medium | Threaded send silently breaks again. | `scripts/verify_dermot_recovery.sh` documents the manual recipe; run before each release. Add a doctor.c health-check that tails `imessage_action.jsonl` and reports `tier_used` distribution (`flat_fallback` ratio > 50% → WARN). |
| **R5** | Re-routing conversational to local stresses Apple Silicon (heat, battery, fan). | Low | Laptop performance impact. | mlx-server already has rate-limiting; user can disable via `mlx_local_enabled: false` for trip-the-breaker reversion. |
| **R6** | The 32-char text prefix matches the wrong message when two messages from Dermot start with the same characters (e.g. two "Yeah"s in a row). | Low | Reply threads to the wrong parent. | Document the limitation; if collision becomes real, escalate the prefix to 64 chars or look up by approximate timestamp+prefix. Out of scope for v1; pin a follow-up task. |

## What ships in what commit

The three components are **independent** and can ship in three separate
commits, in the order C1 → C2 → C3:

- **After C1 alone:** Dermot stops getting the canned template. Replies are
  vanilla Gemini, which is at least conversational. THREADING is still
  broken (flat fallback). Voice is still not Seth.
- **After C1 + C2:** Replies are Seth-voiced AND conversational. Threading
  still broken.
- **After C1 + C2 + C3:** Full recovery.

This is intentional — each commit is independently revertable and ships
visible improvement. Aligns with `~/.claude/rules/agent-team-os.md` task
sizing discipline.

## Approval gate

design.md MUST be approved (or amended) before tasks.md is written.
