# iMessage Action Surface — Design

**Date:** 2026-05-25
**Spec dir:** `docs/plans/2026-05-25-imessage-action-surface/`
**Requirements:** [requirements.md](requirements.md)
**Status:** Draft for review

## Architecture overview

```
                        ┌────────────────────────────────────────────────┐
                        │ Daemon picks reply path per turn               │
                        │   facts = build_facts(parent_msg, conv_state,  │
                        │                       persona)                 │
                        │   style = hu_imessage_choose_reply_style(...)  │
                        └──────────────────┬─────────────────────────────┘
                                           │
                ┌────────────┬─────────────┴────────────┬────────────────┐
                ▼            ▼                          ▼                ▼
        STYLE_FLAT    STYLE_THREADED            STYLE_TAPBACK     STYLE_TAPBACK_PLUS_FLAT
                │            │                          │                │
                │            ▼                          │                ├─ react() then send()
                │   ┌────────────────────┐              │                │
                │   │ vtable->reply()    │              │                │
                │   │  tier 1: Cmd-R     │              │                │
                │   │  tier 2: AX menu   │              │                │
                │   │  tier 3: flat send │◄──── warn ───┤                │
                │   └─────────┬──────────┘              │                │
                │             │                         │                │
                ▼             ▼                         ▼                ▼
              vtable->send()                   vtable->react()    vtable->react()+send()
                                                  (existing,           (sequential)
                                                  extended for
                                                  custom emoji)
                                           │
                                           ├── all paths emit JSONL telemetry ──►
                                           │      ~/.human/logs/imessage_action.jsonl
                                           │
                                           └── sticker path (orthogonal, called
                                               from persona-judgment elsewhere):
                                               vtable->send_sticker(target, path)
                                                 → imsg send --file
```

Three new pieces:

1. **Pure predicate module** (`imessage_action.{h,c}`) — facts struct, style enum, scoring function, seeded sampling. Zero I/O. Lives in `src/channels/`.
2. **Send paths** — extend `react` (custom emoji), add `reply` (threaded), add `send_sticker` (attachment-based). All three go through `src/channels/imessage_reply.c` (reply), `src/channels/imessage_react.c` (react extension, new file by extraction from `imessage.c`), and `src/channels/imessage_sticker.c` (sticker).
3. **Telemetry sink** — single helper `hu_imessage_action_log_jsonl()` in `imessage_action.c` emits the JSONL line. Reused by all three send paths.

## §1. The predicate (the human-likeness core)

### Header (`include/human/channels/imessage_action.h`)

```c
#ifndef HUMAN_CHANNELS_IMESSAGE_ACTION_H
#define HUMAN_CHANNELS_IMESSAGE_ACTION_H

#include "human/core/types.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HU_REPLY_STYLE_FLAT = 0,
    HU_REPLY_STYLE_THREADED,
    HU_REPLY_STYLE_TAPBACK,
    HU_REPLY_STYLE_TAPBACK_PLUS_FLAT,
} hu_reply_style_t;

typedef struct {
    /* Time / position. */
    int64_t seconds_since_parent;
    int     parent_position_from_bottom;   /* 0 = most recent inbound */

    /* Conversational context. */
    int     pending_questions_in_window;   /* unresolved Qs in last 10 inbound msgs */
    int     other_threaded_replies_recent; /* their style — last 20 of their msgs */
    int     our_threaded_replies_recent;   /* our consistency — last 20 outbound */
    float   conv_density_msgs_per_min;     /* over last 5-min window */
    bool    parent_was_a_question;         /* ends in ? or imperative shape */

    /* Persona. */
    float   persona_formality;          /* [0..1] from existing persona.formality */
    float   persona_thread_affinity;    /* [0..1] new persona dial, default 0.3 */

    /* Emotional protection (AC-3). */
    int     parent_emotional_intensity;    /* enum HU_EMOTION_THRESHOLD_* */
} hu_reply_style_facts_t;

/* Pure. No I/O. Deterministic given facts + rng_seed. */
hu_reply_style_t hu_imessage_choose_reply_style(const hu_reply_style_facts_t *facts,
                                                uint64_t rng_seed);

/* Test helper — exposes the underlying score so the truth table can assert
 * the right *probability* not just the sampled style. */
typedef struct {
    float p_thread;
    float p_tapback;
    float p_flat;
    float p_tapback_plus_flat;
} hu_reply_style_scores_t;

hu_reply_style_scores_t hu_imessage_score_reply_style(
    const hu_reply_style_facts_t *facts);

#endif
```

### Implementation sketch (`src/channels/imessage_action.c`)

```c
/* Per-fact threading nudges, expressed as additive log-odds.
 * Tuned for a default thread_affinity=0.3 to land the global thread-rate
 * near 25-30% on representative fact distributions (AC-2). */
static float thread_logodds(const hu_reply_style_facts_t *f) {
    float l = logf(f->persona_thread_affinity / (1.0f - f->persona_thread_affinity + 1e-6f));

    /* Staleness — older parents pull stronger thread. */
    if (f->seconds_since_parent > 600)       l += 1.2f;
    else if (f->seconds_since_parent > 120)  l += 0.4f;
    else if (f->seconds_since_parent < 10)   l -= 0.8f;     /* fresh, no need */

    /* Position — message scrolled off the active view. */
    if (f->parent_position_from_bottom >= 5) l += 0.8f;
    if (f->parent_position_from_bottom >= 10) l += 0.6f;

    /* Pending questions — threading disambiguates which one we answer. */
    if (f->pending_questions_in_window >= 2) l += 0.7f;
    if (f->pending_questions_in_window >= 4) l += 0.5f;

    /* Soft mirror — they thread, we thread (but never always). */
    if (f->other_threaded_replies_recent >= 2) l += 0.6f;
    if (f->other_threaded_replies_recent >= 5) l += 0.4f;

    /* Density — rapid-fire kills threading. */
    if (f->conv_density_msgs_per_min > 6.0f)  l -= 1.0f;
    if (f->conv_density_msgs_per_min > 12.0f) l -= 0.8f;

    /* Question handling. */
    if (f->parent_was_a_question)            l += 0.3f;

    /* Formality — more formal personas thread more. */
    l += (f->persona_formality - 0.5f) * 0.6f;

    return l;
}

static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

hu_reply_style_scores_t hu_imessage_score_reply_style(
    const hu_reply_style_facts_t *f) {
    hu_reply_style_scores_t s = {0};
    float p_thread = sigmoid(thread_logodds(f));

    /* Tapback-only probability: low for substantive replies, higher for
     * casual confirmations. Hard-zeroed by emotional protection (AC-3). */
    float p_tap = 0.15f;
    if (f->parent_was_a_question) p_tap = 0.02f;  /* questions deserve words */
    if (f->conv_density_msgs_per_min > 8.0f) p_tap += 0.10f; /* casual chat */
    if (f->parent_emotional_intensity >= HU_EMOTION_THRESHOLD_MEDIUM) p_tap = 0.0f;

    float p_tap_plus = 0.05f;  /* rare; used for emotional acknowledgment */
    if (f->parent_emotional_intensity >= HU_EMOTION_THRESHOLD_MEDIUM) p_tap_plus = 0.20f;

    /* Normalize tapback-bearing masses out of (1 - p_thread). */
    float p_remaining = 1.0f - p_thread;
    float tap_share = p_tap + p_tap_plus;
    if (tap_share > p_remaining) tap_share = p_remaining;
    float p_flat = p_remaining - tap_share;
    if (tap_share > 0) {
        float scale = (p_remaining > 0 && (p_tap + p_tap_plus) > 0)
                          ? tap_share / (p_tap + p_tap_plus)
                          : 0;
        p_tap *= scale;
        p_tap_plus *= scale;
    }

    s.p_thread = p_thread;
    s.p_tapback = p_tap;
    s.p_flat = p_flat;
    s.p_tapback_plus_flat = p_tap_plus;
    return s;
}

hu_reply_style_t hu_imessage_choose_reply_style(
    const hu_reply_style_facts_t *facts, uint64_t rng_seed) {
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(facts);
    /* Convert seed to [0,1) via xorshift64*. */
    uint64_t x = rng_seed ? rng_seed : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    float r = (float)((x * 0x2545F4914F6CDD1DULL) >> 11) / (float)(1ULL << 53);

    /* Cumulative draw. Order: THREAD, TAPBACK, TAPBACK_PLUS_FLAT, FLAT. */
    float c = 0;
    c += s.p_thread;                if (r < c) return HU_REPLY_STYLE_THREADED;
    c += s.p_tapback;               if (r < c) return HU_REPLY_STYLE_TAPBACK;
    c += s.p_tapback_plus_flat;     if (r < c) return HU_REPLY_STYLE_TAPBACK_PLUS_FLAT;
    return HU_REPLY_STYLE_FLAT;
}
```

The exact log-odds weights are starting values — AC-2 measures the distribution and AC-8 telemetry surfaces real-world drift; we tune from there.

## §2. Vtable changes

```c
/* include/human/channel.h additions */
struct hu_channel_vtable_t {
    /* ... existing slots ... */

    /* NEW: threaded reply. parent_msg_guid is the guid of the inbound message
     * we're replying to. Channels without threading return HU_ERR_NOT_SUPPORTED. */
    hu_error_t (*reply)(void *ctx, const char *target, size_t target_len,
                        const char *parent_msg_guid, const char *body,
                        const hu_message_options_t *opts);

    /* EXTENDED: react() already exists. Add an extra slot rather than
     * breaking the signature: react_emoji() takes an arbitrary UTF-8 emoji
     * codepoint. The existing enum-based react() stays for the 6 classics. */
    hu_error_t (*react_emoji)(void *ctx, const char *target, size_t target_len,
                              int64_t message_id, const char *emoji_utf8);

    /* NEW: sticker send. sticker_path is an absolute file path. */
    hu_error_t (*send_sticker)(void *ctx, const char *target, size_t target_len,
                               const char *sticker_path);
};
```

The 42 other channels get a one-line stub returning `HU_ERR_NOT_SUPPORTED` for each new slot. Per `~/.claude/rules/agent-task-sizing.md`, wiring those stubs is a script-friendly mechanical edit, not an agent task.

## §3. Reply send path — 3-tier escalation

`src/channels/imessage_reply.c::hu_imessage_reply()`:

```
1. Tier 1: Cmd-R after AX row focus
   - ax_open_conversation(target)
   - ax_find_message_group(parent_guid_or_content_prefix) → focus row
   - AX press Cmd-R on focused row
   - if composer field appears: type body, press Return
   - elapsed ~200-400 ms
   - on success: log telemetry, return HU_OK

2. Tier 2: AXShowMenu → "Reply…"
   - same focus, then AXShowMenu
   - locate menu item whose title startswith "Reply" (handles ellipsis variants)
   - click it
   - wait for composer (poll for AX text field appearance, 1s budget)
   - type body, press Return
   - elapsed ~700-1000 ms
   - on success: log telemetry, return HU_OK

3. Tier 3: flat send (current behavior)
   - call vtable->send(target, body)
   - log WARN: "reply degraded to flat (parent=%s reason=%s)"
   - log telemetry with tier_used="flat_fallback"
   - return HU_OK (caller sees a successful send, just unthreaded)
```

Parent identification: we already have `reply_to_guid` on inbound messages
(see [imessage.h:388](../../include/human/channels/imessage.h)). The caller of `reply()` passes the inbound's own guid as `parent_msg_guid`; we use it both to locate the AX row (cross-referenced via chat.db lookup of `text` for prefix matching, since AX rows don't directly expose guid) AND as the value chat.db will store in the outbound row's `reply_to_guid` once Messages.app processes the AX click.

## §4. Custom-emoji tapback (extend react)

The current `ax_perform_tapback_on_row` maps the 6 classic enum tapbacks to AX action names like "Loved", "Liked". For custom emoji we need to:

1. Navigate the same AXShowMenu
2. Locate the "Add Reaction" / custom-emoji sub-picker (Sonoma+ shows a row of 6 below the classics)
3. Match the picker child whose AXValue codepoint matches the requested emoji
4. If not found in the visible row, click the "+ More" button and search the emoji picker (out of scope this sprint — limit to the 6 visible ones)

Fallback map (configurable; defaults in `src/channels/imessage_react.c`):

```c
static const struct { const char *emoji; const char *classic_label; } CLASSIC_MAP[] = {
    {"❤️", "Loved"}, {"♥️", "Loved"}, {"💕", "Loved"}, {"💗", "Loved"},
    {"👍", "Liked"}, {"👍🏻", "Liked"}, {"👍🏼", "Liked"}, {"👍🏽", "Liked"},
    {"👎", "Disliked"},
    {"😂", "Laughed"}, {"🤣", "Laughed"}, {"😆", "Laughed"},
    {"‼️", "Emphasized"}, {"❗", "Emphasized"},
    {"❓", "Questioned"}, {"❔", "Questioned"},
    {NULL, NULL}
};
```

If a custom emoji isn't in the visible sub-picker row AND isn't in `CLASSIC_MAP`, return `HU_ERR_NOT_SUPPORTED` so the caller can fall back to a flat text reply containing the emoji.

## §5. Sticker pipeline (MVP)

**The honest constraint:** Apple's native sticker pipeline uses `IMSticker` from the private `IMSharedUtilities` framework, routed through the same `IMCore` daemon connection we already discovered is locked down on macOS 26+ ([imessage.c:3637](../../src/channels/imessage.c)). We cannot send a true balloon-bundle sticker without an entitlement we won't get.

**MVP that works today:** sticker files live in `~/.human/stickers/` as PNG/HEIC with tagged filenames like `casual-happy_001.png`, `formal-acknowledgment_002.png`. The persona picker `hu_persona_pick_sticker` returns a path; the send goes through `imsg send --to X --file <path>`. Recipients see a small image attachment.

```
~/.human/stickers/
├── casual-happy_001.png
├── casual-happy_002.png
├── formal-acknowledgment_001.png
├── humor-laugh_001.png
└── README.md   ← documents the tag schema for users
```

**Naming schema:** `<context>-<mood>_<seq>.{png,heic}` where:
- `<context>` ∈ {`casual`, `formal`, `intimate`, `playful`}
- `<mood>` ∈ {`happy`, `acknowledgment`, `laugh`, `support`, `apology`, `gratitude`}
- `<seq>` ∈ `001..999`

Picker logic: filter by inferred context-mood from the conversational context (per AC-6 fixture), uniform-sample within the filtered set, weighted slightly toward less-recently-used filenames (rotation).

**No sticker dir? No problem.** The vtable method returns `HU_ERR_NOT_SUPPORTED` if the dir is empty or missing. The caller's natural reaction is to fall back to a flat text reply or tapback. No hard failures.

**Honest caveat (documented in user-facing docs):** "Stickers send as attachments, not as native Apple stickers. Most recipients won't notice; some power users will see the difference in the bubble shape."

## §6. Latency budget + jitter

The reply path is wrapped in `hu_persona_pace_reply(persona, start_ts)`:

```c
void hu_persona_pace_reply(const hu_persona_t *persona, int64_t start_ts) {
    int64_t min_delay_ms = persona->min_reply_delay_ms;       /* e.g. 1500 */
    int64_t variance_ms  = persona->reply_delay_variance_ms;  /* e.g. 600  */
    int64_t jitter = arc4random_uniform((uint32_t)variance_ms * 2) - variance_ms;
    int64_t target_total = min_delay_ms + jitter;
    int64_t elapsed = now_ms() - start_ts;
    if (elapsed < target_total) {
        usleep((useconds_t)((target_total - elapsed) * 1000));
    }
}
```

AC-7 specifies `>= persona.min_reply_delay_ms * 1.2`. The 0.2× margin protects against the rare case where AX itself was slower than min_delay (then we don't add jitter at all — already paid the cost).

Existing `hu_persona_t` already has `min_reply_delay_ms`; we add `reply_delay_variance_ms` (default 600).

## §7. Telemetry — every decision logged

Every call site:

```c
hu_imessage_action_log_jsonl(&(hu_imessage_action_log_t){
    .ts_unix = now_unix(),
    .target_chat_id_hash = hash_chat_id(target, target_len),
    .facts = facts,
    .style_chosen = style,
    .send_result = err,
    .tier_used = tier,  /* "cmdR" | "ax_menu" | "flat_fallback" | "tapback" */
    .elapsed_ms = elapsed_ms,
});
```

JSONL line:

```json
{"ts":1716681234,"chat":"a3f1...","facts":{"sec_since_parent":47,"density":3.2,"thread_aff":0.30,"emo":1,"q":true,"o_th":1,"u_th":0,"pos":1,"pq":1,"formality":0.4},"style":"THREADED","result":"OK","tier":"ax_menu","elapsed_ms":812}
```

Lands in `~/.human/logs/imessage_action.jsonl`. Rotated daily by existing log rotation. Mined by future DPO/tuning sprints to refine the log-odds weights.

## §8. Configuration

New keys in `~/.human/config.json`:

```json
{
  "iMessage": {
    "action_surface_v2": {
      "enabled": true,                    // default true on macOS, false elsewhere
      "thread_affinity_default": 0.3,
      "min_reply_delay_ms": 1500,
      "reply_delay_variance_ms": 600,
      "sticker_dir": "~/.human/stickers"
    }
  }
}
```

Per [silent-config-gated-subsystems.md](../../../.claude/rules/silent-config-gated-subsystems.md): on first invocation when disabled, emit `hu_log_info_once("imessage", NULL, "action_surface_v2 disabled by config (iMessage.action_surface_v2.enabled=false); set to true to enable threaded replies / custom tapbacks / stickers")`.

## §9. Tests — file layout

```
tests/test_imessage_reply_style.c          # truth table (12 cases) + distribution shape (100 cases)
tests/test_imessage_threaded_reply.c       # AX mock harness, 3-tier escalation
tests/test_imessage_custom_tapback.c       # 6 emoji + 1 fallback
tests/test_imessage_sticker.c              # fake sticker dir, picker, attachment send
tests/test_imessage_reply_pacing.c         # latency jitter (AC-7)
tests/test_imessage_action_telemetry.c     # JSONL line shape
tests/fixtures/imessage_action/            # 100 synthetic fact-tuples (AC-2)
```

Per [test-source-gate-symmetry.md](../../../.claude/rules/test-source-gate-symmetry.md): all new tests are wrapped in `#ifdef HU_ENABLE_IMESSAGE` or use the stub-runner pattern; CMakeLists symmetry must be maintained.

Per [test-references-production-symbol.md](../../../.claude/rules/test-references-production-symbol.md): each test file references at least one `hu_imessage_*` symbol so the check-test-references hook passes.

## §10. Build / CI considerations

- **macOS-only AX path:** all AX code stays in the existing `#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)` guard. Linux CI builds the predicate-only path (compiles `imessage_action.c` for the predicate; stubs out reply/react_emoji/send_sticker).
- **No new dependencies:** all of this is libc + existing CoreFoundation/Accessibility frameworks already linked via tapback.
- **Linker:** no new framework links needed.
- **Test latency:** AC-7 has `usleep` calls — wrap with `HU_IS_TEST` so the test harness can short-circuit the sleep when injecting clock.

## §11. Migration / rollout

- **Default behavior change:** with `action_surface_v2.enabled=true` (the macOS default), some replies that previously sent flat will now thread. This is the intended product change.
- **Easy rollback:** set `iMessage.action_surface_v2.enabled=false` in config.json; daemon logs the disable, all behavior reverts to today's flat-send.
- **First-week telemetry review:** after deploy, sample 100 random JSONL lines and grade style fit by hand. Tune log-odds weights if the distribution looks off.

## §12. Open questions for design review

1. **Sticker dir location** — `~/.human/stickers/` OK, or should it be `~/.human/config/stickers/` to keep all user-tunable assets together?
2. **Tag schema** — is `<context>-<mood>_<seq>` granular enough? Should we add `<tone>` (e.g. `_warm`, `_dry`)?
3. **Persona dial naming** — sticking with `persona_thread_affinity` from the brainstorm, OR renaming to something a non-developer would set: `persona.tendency_to_thread_replies` (verbose but plain)?
4. **react_emoji vs reaction overload** — separate vtable slot (current proposal) keeps the existing 6-classic enum react() pristine; or fold both into one `react()` that takes an `enum {CLASSIC, CUSTOM}` discriminator + union. I lean separate slot; flag if you'd prefer one.
