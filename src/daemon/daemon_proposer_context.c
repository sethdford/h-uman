/* src/daemon/daemon_proposer_context.c — HU_PROPOSER_CONTEXT.
 *
 * The per-contact proposer decides "text this person now?" from a briefing that
 * never contained the thread with them (2026-10-01: 111 of 200 per-contact
 * proposer calls declined). This adds the contact profile, the last
 * HU_PROPOSER_CTX_THREAD_TURNS messages and the contact_insights stream, and
 * keeps all of it on a loopback model: see include/human/daemon/proposer_context.h.
 *
 * Activation gated on the shadow measurement in docs/guides/proposer-context.md
 * (would-propose rate per contact-cycle, distinct contacts, blind check of ~20
 * drafts): do not flip to live without it. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* strptime */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "human/daemon/proposer_context.h"

#include "human/agent.h"
#include "human/agent/memory_loader.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/providers/compatible.h"
#include "human/providers/reliable.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_insights_repo.h"
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_PROPOSER_CTX_INSIGHT_ITEMS 6

/* ── gate ─────────────────────────────────────────────────────────────── */

static int s_mode_override = -1;

hu_gate_mode_t hu_proposer_context_mode(void) {
    if (s_mode_override >= 0)
        return (hu_gate_mode_t)s_mode_override;
    return hu_gate_mode_from_env("HU_PROPOSER_CONTEXT", HU_GATE_OFF);
}

void hu_proposer_context_set_mode_for_test(int mode) {
    s_mode_override = mode;
}

static const char *mode_name(hu_gate_mode_t m) {
    return m == HU_GATE_LIVE ? "live" : m == HU_GATE_SHADOW ? "shadow" : "off";
}

/* ── local provider ───────────────────────────────────────────────────── */

bool hu_proposer_context_local_provider(const hu_provider_t *p, hu_provider_t *out) {
    if (!p || !p->vtable || !out)
        return false;
    hu_provider_t cand = *p;
    hu_provider_t prim = {0};
    hu_error_t rerr = hu_reliable_primary(p, &prim);
    if (rerr == HU_OK)
        cand = prim; /* the primary alone — the wrapper would fail over */
    else if (rerr != HU_ERR_INVALID_ARGUMENT)
        return false; /* a reliable wrapper whose circuit is open: fallbacks are serving */
    if (!hu_compatible_is_loopback(&cand))
        return false;
    *out = cand;
    return true;
}

void hu_proposer_context_begin_with_local(hu_proposer_context_t *pc, hu_gate_mode_t mode,
                                          const hu_provider_t *local) {
    if (!pc)
        return;
    memset(pc, 0, sizeof(*pc));
    pc->mode = mode;
    pc->days_since_last = -1;
    pc->days_since_inbound = -1;
    if (mode != HU_GATE_OFF && local && local->vtable) {
        pc->local = *local;
        pc->local_ok = true;
    }
}

void hu_proposer_context_begin(hu_proposer_context_t *pc, const hu_provider_t *agent_provider,
                               const hu_channel_history_entry_t *entries, size_t n,
                               int64_t now_unix) {
    hu_gate_mode_t mode = hu_proposer_context_mode();
    hu_provider_t local = {0};
    bool ok = false;
#if !HU_IS_TEST
    /* Test builds never resolve a real provider; tests use begin_with_local. */
    if (mode != HU_GATE_OFF)
        ok = hu_proposer_context_local_provider(agent_provider, &local);
#else
    (void)agent_provider;
#endif
    if (mode != HU_GATE_OFF) {
        static atomic_bool announced = false;
        hu_log_info_once(&announced, "proposer_context", NULL,
                         "proposer context active: mode=%s local_provider=%s (set "
                         "HU_PROPOSER_CONTEXT=off to disable)",
                         mode_name(mode), ok ? "loopback" : "none — thread is never built");
    }
    hu_proposer_context_begin_with_local(pc, mode, ok ? &local : NULL);
    hu_proposer_context_capture_thread(pc, entries, n, now_unix);
}

/* ── thread ───────────────────────────────────────────────────────────── */

static bool parse_local_ts(const char *ts, int64_t *out) {
    if (!ts || !ts[0])
        return false;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (!strptime(ts, "%Y-%m-%d %H:%M", &tmv))
        return false;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1)
        return false;
    *out = (int64_t)t;
    return true;
}

static void relative_label(int64_t delta, char *buf, size_t cap) {
    if (delta < 0)
        delta = 0;
    if (delta < 90)
        snprintf(buf, cap, "just now");
    else if (delta < 3600)
        snprintf(buf, cap, "%lldm ago", (long long)(delta / 60));
    else if (delta < 86400)
        snprintf(buf, cap, "%lldh ago", (long long)(delta / 3600));
    else
        snprintf(buf, cap, "%lldd ago", (long long)(delta / 86400));
}

/* One message as "[<when>] <Seth|them>: <text>\n" — text flattened to one line
 * and capped at HU_PROPOSER_CTX_LINE_MAX bytes on a UTF-8 boundary. */
static size_t render_line(const hu_channel_history_entry_t *e, int64_t now, char *out, size_t cap) {
    char when[24] = "";
    int64_t ts = 0;
    if (parse_local_ts(e->timestamp, &ts))
        relative_label(now - ts, when, sizeof(when));
    size_t tlen = strnlen(e->text, sizeof(e->text));
    if (tlen > HU_PROPOSER_CTX_LINE_MAX) {
        tlen = HU_PROPOSER_CTX_LINE_MAX;
        while (tlen > 0 && ((unsigned char)e->text[tlen] & 0xC0) == 0x80)
            tlen--; /* never split a multi-byte character */
    }
    char text[HU_PROPOSER_CTX_LINE_MAX + 1];
    for (size_t i = 0; i < tlen; i++) {
        char c = e->text[i];
        text[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    text[tlen] = '\0';
    int n = when[0] ? snprintf(out, cap, "[%s] %s: %s\n", when, e->from_me ? "Seth" : "them", text)
                    : snprintf(out, cap, "%s: %s\n", e->from_me ? "Seth" : "them", text);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t hu_proposer_context_render_thread(const hu_channel_history_entry_t *entries, size_t n,
                                         int64_t now_unix, char *buf, size_t cap,
                                         int64_t *out_days_since_last,
                                         int64_t *out_days_since_inbound) {
    if (out_days_since_last)
        *out_days_since_last = -1;
    if (out_days_since_inbound)
        *out_days_since_inbound = -1;
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!entries || n == 0)
        return 0;

    /* Days since the newest message, and since their newest message. */
    for (size_t i = n; i > 0; i--) {
        int64_t ts = 0;
        if (!parse_local_ts(entries[i - 1].timestamp, &ts))
            continue;
        int64_t days = now_unix > ts ? (now_unix - ts) / 86400 : 0;
        if (out_days_since_last && *out_days_since_last < 0)
            *out_days_since_last = days;
        if (out_days_since_inbound && *out_days_since_inbound < 0 && !entries[i - 1].from_me)
            *out_days_since_inbound = days;
    }

    /* Newest lines that fit, written oldest-first. */
    size_t first = n > HU_PROPOSER_CTX_THREAD_TURNS ? n - HU_PROPOSER_CTX_THREAD_TURNS : 0;
    char lines[HU_PROPOSER_CTX_THREAD_TURNS][HU_PROPOSER_CTX_LINE_MAX + 48];
    size_t lens[HU_PROPOSER_CTX_THREAD_TURNS];
    size_t count = n - first;
    for (size_t i = 0; i < count; i++)
        lens[i] = render_line(&entries[first + i], now_unix, lines[i], sizeof(lines[i]));
    size_t keep_from = count, total = 0;
    while (keep_from > 0 && total + lens[keep_from - 1] < cap) {
        total += lens[keep_from - 1];
        keep_from--;
    }
    size_t pos = 0;
    for (size_t i = keep_from; i < count; i++) {
        memcpy(buf + pos, lines[i], lens[i]);
        pos += lens[i];
    }
    buf[pos] = '\0';
    return pos;
}

void hu_proposer_context_capture_thread(hu_proposer_context_t *pc,
                                        const hu_channel_history_entry_t *entries, size_t n,
                                        int64_t now_unix) {
    if (!pc || pc->mode == HU_GATE_OFF || !pc->local_ok)
        return; /* the text never leaves the history buffer otherwise */
    pc->thread_len =
        hu_proposer_context_render_thread(entries, n, now_unix, pc->thread, sizeof(pc->thread),
                                          &pc->days_since_last, &pc->days_since_inbound);
}

/* ── contact + memory + block ─────────────────────────────────────────── */

/* Copy whole lines of src into dst (cap bytes incl. NUL), skipping any line
 * that does not fit and the builder's own "--- Contact profile for <id> ---"
 * header (it names the number; our block carries its own header). */
static size_t copy_fitting_lines(char *dst, size_t cap, const char *src, size_t len) {
    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && src[j] != '\n')
            j++;
        size_t line = j - i;
        bool header = line >= 3 && memcmp(src + i, "---", 3) == 0;
        if (line > 0 && !header && pos + line + 1 < cap) {
            memcpy(dst + pos, src + i, line);
            pos += line;
            dst[pos++] = '\n';
        }
        i = j + 1;
    }
    dst[pos] = '\0';
    return pos;
}

static size_t append(char *buf, size_t cap, size_t pos, const char *s, size_t len) {
    if (pos + 1 >= cap)
        return pos;
    size_t room = cap - 1 - pos;
    size_t n = len < room ? len : room;
    memcpy(buf + pos, s, n);
    pos += n;
    buf[pos] = '\0';
    return pos;
}

void hu_proposer_context_build(hu_proposer_context_t *pc, hu_allocator_t *alloc,
                               hu_memory_t *memory, const struct hu_contact_profile *cp,
                               bool (*content_is_safe)(const char *, size_t)) {
    if (!pc || pc->mode == HU_GATE_OFF || !pc->local_ok)
        return;
    pc->contact_len = 0;
    pc->memory_len = 0;
    pc->block_len = 0;
    pc->block[0] = '\0';

    if (alloc && cp) {
        char *ctx = NULL;
        size_t ctx_len = 0;
        if (hu_contact_profile_build_context(alloc, cp, &ctx, &ctx_len) == HU_OK && ctx) {
            pc->contact_len = copy_fitting_lines(pc->contact, sizeof(pc->contact), ctx, ctx_len);
            alloc->free(alloc->ctx, ctx, ctx_len + 1);
        }
    }
#ifdef HU_ENABLE_SQLITE
    if (alloc && memory && cp && cp->contact_id) {
        char *lines = NULL;
        size_t lines_len = 0;
        if (hu_contact_insights_render(memory, alloc, cp->contact_id, strlen(cp->contact_id),
                                       HU_PROPOSER_CTX_INSIGHT_ITEMS, sizeof(pc->memory) - 1,
                                       HU_INSIGHT_MIN_CONFIDENCE, &lines, &lines_len) == HU_OK &&
            lines && lines_len > 0) {
            if (!content_is_safe || content_is_safe(lines, lines_len))
                pc->memory_len = append(pc->memory, sizeof(pc->memory), 0, lines, lines_len);
            alloc->free(alloc->ctx, lines, lines_len + 1);
        }
    }
#else
    (void)memory;
    (void)content_is_safe;
#endif

    char *b = pc->block;
    const size_t cap = sizeof(pc->block);
    size_t pos = 0;
    if (pc->contact_len > 0) {
        static const char h[] = "\n--- contact profile ---\n";
        pos = append(b, cap, pos, h, sizeof(h) - 1);
        pos = append(b, cap, pos, pc->contact, pc->contact_len);
    }
    if (pc->thread_len > 0) {
        char h[128];
        int n;
        if (pc->days_since_last < 0)
            n = snprintf(h, sizeof(h), "\n--- recent thread ---\n");
        else if (pc->days_since_inbound >= 0 && pc->days_since_inbound != pc->days_since_last)
            n = snprintf(h, sizeof(h),
                         "\n--- recent thread (last contact %lld days ago; their last message "
                         "%lld days ago) ---\n",
                         (long long)pc->days_since_last, (long long)pc->days_since_inbound);
        else
            n = snprintf(h, sizeof(h), "\n--- recent thread (last contact %lld days ago) ---\n",
                         (long long)pc->days_since_last);
        if (n > 0 && (size_t)n < sizeof(h))
            pos = append(b, cap, pos, h, (size_t)n);
        pos = append(b, cap, pos, pc->thread, pc->thread_len);
    }
    if (pc->memory_len > 0) {
        static const char h[] = "\n--- what you remember about them ---\n";
        pos = append(b, cap, pos, h, sizeof(h) - 1);
        pos = append(b, cap, pos, pc->memory, pc->memory_len);
    }
    pc->block_len = pos;
}

/* ── shadow rate limit: one enriched call per contact per proposer cycle ─ */

#define HU_PROPOSER_CTX_RL_SLOTS 32
static struct {
    uint64_t key;
    int64_t cycle;
} s_rl[HU_PROPOSER_CTX_RL_SLOTS];

static uint64_t contact_hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static bool rate_allow(uint64_t key, int64_t cycle) {
    size_t victim = 0;
    for (size_t i = 0; i < HU_PROPOSER_CTX_RL_SLOTS; i++) {
        if (s_rl[i].key == key) {
            if (s_rl[i].cycle == cycle)
                return false;
            s_rl[i].cycle = cycle;
            return true;
        }
        if (s_rl[i].key == 0 || s_rl[i].cycle < s_rl[victim].cycle)
            victim = i;
        if (s_rl[i].key == 0)
            break;
    }
    s_rl[victim].key = key;
    s_rl[victim].cycle = cycle;
    return true;
}

void hu_proposer_context_reset_rate_limit_for_test(void) {
    memset(s_rl, 0, sizeof(s_rl));
}

/* ── decide ───────────────────────────────────────────────────────────── */

static hu_proposer_tick_fn s_tick_fn;

void hu_proposer_context_set_tick_fn_for_test(hu_proposer_tick_fn fn) {
    s_tick_fn = fn;
}

static const char *outcome_name(hu_proposer_ctx_outcome_t o) {
    switch (o) {
    case HU_PROPOSER_CTX_OK:
        return "ok";
    case HU_PROPOSER_CTX_LOCAL_UNAVAILABLE:
        return "local_unavailable";
    case HU_PROPOSER_CTX_RATE_LIMITED:
        return "rate_limited";
    case HU_PROPOSER_CTX_NOT_REACHED:
        return "not_reached";
    case HU_PROPOSER_CTX_EMPTY:
        return "empty";
    default:
        return "none";
    }
}

/* Did the production call get as far as asking the model? */
static bool reached_model(hu_init_proposer_result_t r) {
    return r == HU_INIT_RESULT_FIRED || r == HU_INIT_RESULT_LOW_CONFIDENCE ||
           r == HU_INIT_RESULT_NEGATIVE || r == HU_INIT_RESULT_GUARD_REJECT ||
           r == HU_INIT_RESULT_PARSE_ERROR || r == HU_INIT_RESULT_LLM_ERROR;
}

/* ONE aggregate line per event: byte counts, enums and the contact's index in
 * the persona's contact list (stable across restarts, meaningless outside this
 * machine) — never message text, drafts, reasons, names or numbers. */
static void log_event(const hu_proposer_context_t *pc, int contact_idx,
                      hu_init_proposer_result_t prod, int local_err, bool should_propose,
                      double confidence, size_t reason_len) {
    hu_log_info("proposer_context", NULL,
                "[HU_PROPOSER_CONTEXT %s] outcome=%s contact_idx=%d contact_bytes=%zu "
                "conversation_bytes=%zu memory_bytes=%zu block_bytes=%zu days_since=%lld "
                "prod_result=%d local_err=%d should_propose=%d confidence=%.3f reason_len=%zu",
                mode_name(pc->mode), outcome_name(pc->outcome), contact_idx, pc->contact_len,
                pc->thread_len, pc->memory_len, pc->block_len, (long long)pc->days_since_last,
                (int)prod, local_err, should_propose ? 1 : 0, confidence, reason_len);
}

void hu_proposer_context_decide(hu_proposer_context_t *pc, const struct hu_initiative_config *cfg,
                                const struct hu_autoresponder_config *ar_cfg, int32_t tz_offset_s,
                                struct hu_proactive_budget *budget, struct hu_agent *agent,
                                hu_provider_t *provider, hu_allocator_t *alloc,
                                const struct hu_contact_profile *cp,
                                const hu_proactive_compose_inputs_t *inputs, int64_t now_unix,
                                hu_init_proposer_result_t *out_result,
                                hu_init_decision_t *out_decision) {
    hu_proposer_tick_fn tick = s_tick_fn ? s_tick_fn : hu_init_proposer_tick_with_provider_ex;
    hu_init_proposer_result_t result = HU_INIT_RESULT_SKIP;
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_gate_mode_t mode = pc ? pc->mode : HU_GATE_OFF;
    if (pc)
        pc->outcome = HU_PROPOSER_CTX_NONE;

    if (mode == HU_GATE_OFF || !inputs) { /* today's call, unchanged */
        (void)tick(cfg, ar_cfg, tz_offset_s, budget, agent, provider, alloc, inputs, 0, now_unix,
                   &last_tick, &tick_id, &result, out_decision);
        if (out_result)
            *out_result = result;
        return;
    }

    hu_proposer_context_build(pc, alloc, agent ? agent->memory : NULL, cp, inputs->content_is_safe);
    hu_proactive_compose_inputs_t rich = *inputs;
    rich.proposer_context = pc->block;
    rich.proposer_context_len = pc->block_len;
    bool have_block = pc->local_ok && pc->block_len > 0;
    uint64_t key = contact_hash(inputs->contact_id ? inputs->contact_id : "",
                                inputs->contact_id ? inputs->contact_id_len : 0);
    int idx = -1;
    if (agent && agent->persona && cp && cp >= agent->persona->contacts &&
        cp < agent->persona->contacts + agent->persona->contacts_count)
        idx = (int)(cp - agent->persona->contacts);

    if (mode == HU_GATE_LIVE) {
        if (have_block) {
            /* The local provider itself — never the reliable chain. */
            (void)tick(cfg, ar_cfg, tz_offset_s, budget, agent, &pc->local, alloc, &rich, 0,
                       now_unix, &last_tick, &tick_id, &result, out_decision);
            if (result != HU_INIT_RESULT_LLM_ERROR && result != HU_INIT_RESULT_PARSE_ERROR) {
                pc->outcome = HU_PROPOSER_CTX_OK;
                log_event(pc, idx, result, 0, result == HU_INIT_RESULT_FIRED, 0.0, 0);
                if (out_result)
                    *out_result = result;
                return;
            }
            pc->outcome = HU_PROPOSER_CTX_LOCAL_UNAVAILABLE;
        } else {
            pc->outcome = pc->local_ok ? HU_PROPOSER_CTX_EMPTY : HU_PROPOSER_CTX_LOCAL_UNAVAILABLE;
        }
        /* Fall back to today's prompt — the block is NOT carried over. */
        last_tick = 0;
        tick_id = 0;
        (void)tick(cfg, ar_cfg, tz_offset_s, budget, agent, provider, alloc, inputs, 0, now_unix,
                   &last_tick, &tick_id, &result, out_decision);
        log_event(pc, idx, result, 0, false, 0.0, 0);
        if (out_result)
            *out_result = result;
        return;
    }

    /* SHADOW: today's call decides; the enriched prompt is only observed. */
    (void)tick(cfg, ar_cfg, tz_offset_s, budget, agent, provider, alloc, inputs, 0, now_unix,
               &last_tick, &tick_id, &result, out_decision);
    if (out_result)
        *out_result = result;

    hu_init_decision_t shadow;
    memset(&shadow, 0, sizeof(shadow));
    int local_err = 0;
    if (!reached_model(result))
        pc->outcome = HU_PROPOSER_CTX_NOT_REACHED;
    else if (!pc->local_ok)
        pc->outcome = HU_PROPOSER_CTX_LOCAL_UNAVAILABLE;
    else if (pc->block_len == 0)
        pc->outcome = HU_PROPOSER_CTX_EMPTY;
    else if (!rate_allow(key, now_unix))
        pc->outcome = HU_PROPOSER_CTX_RATE_LIMITED;
    else {
        const struct hu_initiative_config *ic = cfg;
        const char *model = (ic && ic->propose_model) ? ic->propose_model : "";
        hu_error_t err =
            hu_init_proposer_decide_once(alloc, &pc->local, model, &rich, now_unix, 0, &shadow);
        local_err = (int)err;
        pc->outcome = err == HU_OK ? HU_PROPOSER_CTX_OK : HU_PROPOSER_CTX_LOCAL_UNAVAILABLE;
    }
    pc->shadow_should_propose = pc->outcome == HU_PROPOSER_CTX_OK && shadow.should_propose;
    pc->shadow_confidence = pc->outcome == HU_PROPOSER_CTX_OK ? shadow.confidence : 0.0;
    log_event(pc, idx, result, local_err, pc->shadow_should_propose, pc->shadow_confidence,
              pc->outcome == HU_PROPOSER_CTX_OK ? shadow.skip_reason_len : 0);
}
