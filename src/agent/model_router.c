#include "human/agent/model_router.h"
#include "human/provider.h"
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static bool ci_contains(const char *haystack, size_t haystack_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > haystack_len)
        return false;
    for (size_t i = 0; i <= haystack_len - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool has_question(const char *msg, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (msg[i] == '?')
            return true;
    return false;
}

static size_t word_count(const char *msg, size_t len) {
    if (!msg || len == 0)
        return 0;
    size_t count = 1;
    for (size_t i = 0; i < len; i++)
        if (msg[i] == ' ')
            count++;
    return count;
}

/* Detect emotional weight — messages that need empathy, not speed */
static int emotional_weight(const char *msg, size_t len) {
    int score = 0;
    static const char *heavy[] = {
        "died", "dying", "cancer", "funeral", "divorce", "breakup", "depressed",
        "suicidal", "hospital", "emergency", "scared", "terrified", "heartbroken",
        "lost my", "passed away", "miss you", "love you", "worried about",
        "don't know what to do", "need help", "struggling", "overwhelmed",
        "can't sleep", "crying", "panic", "anxiety", "therapy"
    };
    static const char *moderate[] = {
        "frustrated", "stressed", "upset", "annoyed", "confused", "angry",
        "disappointed", "tired", "exhausted", "sick", "hurt", "lonely",
        "nervous", "worried", "sorry", "ugh", "hate", "awful", "terrible",
        "not working", "broken", "failing"
    };
    for (size_t i = 0; i < sizeof(heavy) / sizeof(heavy[0]); i++)
        if (ci_contains(msg, len, heavy[i]))
            score += 3;
    for (size_t i = 0; i < sizeof(moderate) / sizeof(moderate[0]); i++)
        if (ci_contains(msg, len, moderate[i]))
            score += 1;
    return score;
}

/* Detect advice-seeking or complex reasoning needs */
static bool needs_reasoning(const char *msg, size_t len) {
    static const char *markers[] = {
        "should i", "what do you think", "what would you", "how do i",
        "help me decide", "pros and cons", "advice", "opinion",
        "compared to", "better option", "worth it", "trade-off",
        "explain", "why does", "how does", "what if"
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
        if (ci_contains(msg, len, markers[i]))
            return true;
    return false;
}

/* Relationship closeness: family and close friends get higher-quality models */
static int relationship_weight(const char *rel, size_t rel_len) {
    if (!rel || rel_len == 0)
        return 0;
    if (rel_len == 6 && memcmp(rel, "family", 6) == 0)
        return 3;
    if (rel_len >= 5 && memcmp(rel, "close", 5) == 0)
        return 3;
    if ((rel_len == 3 && memcmp(rel, "mom", 3) == 0) ||
        (rel_len == 3 && memcmp(rel, "dad", 3) == 0) ||
        (rel_len == 6 && memcmp(rel, "mother", 6) == 0) ||
        (rel_len == 6 && memcmp(rel, "father", 6) == 0) ||
        (rel_len == 6 && memcmp(rel, "sister", 6) == 0) ||
        (rel_len == 7 && memcmp(rel, "brother", 7) == 0) ||
        (rel_len == 6 && memcmp(rel, "spouse", 6) == 0) ||
        (rel_len == 7 && memcmp(rel, "partner", 7) == 0) ||
        (rel_len == 4 && memcmp(rel, "wife", 4) == 0) ||
        (rel_len == 7 && memcmp(rel, "husband", 7) == 0) ||
        (rel_len == 10 && memcmp(rel, "girlfriend", 10) == 0) ||
        (rel_len == 9 && memcmp(rel, "boyfriend", 9) == 0))
        return 3;
    if (rel_len == 6 && memcmp(rel, "friend", 6) == 0)
        return 2;
    if (rel_len == 7 && memcmp(rel, "trusted", 7) == 0)
        return 2;
    if (rel_len == 8 && memcmp(rel, "familiar", 8) == 0)
        return 1;
    return 0;
}

hu_model_router_config_t hu_model_router_default_config(void) {
    hu_model_router_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reflexive_model = "gemini-3.1-flash-lite-preview";
    cfg.reflexive_model_len = 29;
    cfg.conversational_model = "gemini-3-flash-preview";
    cfg.conversational_model_len = 22;
    cfg.analytical_model = "gemini-3-flash-preview";
    cfg.analytical_model_len = 22;
    cfg.deep_model = "gemini-3.1-pro-preview";
    cfg.deep_model_len = 22;
    cfg.on_device_model = "apple-foundationmodel";
    cfg.on_device_model_len = 21;
    cfg.on_device_available = false;
    return cfg;
}

bool hu_model_router_on_device_suitable(hu_cognitive_tier_t tier) {
    return tier == HU_TIER_REFLEXIVE;
}

/* Internal heuristic scoring — returns the raw score for use by both
 * hu_model_route and hu_model_route_with_judge. */
static int compute_heuristic_score(const char *msg, size_t msg_len,
                                   const char *relationship, size_t relationship_len,
                                   int hour, size_t history_count) {
    size_t words = word_count(msg, msg_len);
    int emotion = emotional_weight(msg, msg_len);
    bool question = has_question(msg, msg_len);
    bool reasoning = needs_reasoning(msg, msg_len);
    int rel_w = relationship_weight(relationship, relationship_len);

    int score = 0;

    if (words <= 3)
        score -= 2;
    else if (words <= 8)
        score -= 1;
    else if (words > 80)
        score += 2;
    else if (words > 30)
        score += 1;

    score += emotion;

    if (emotion >= 3)
        score += 3;

    if (question)
        score += 1;
    if (reasoning)
        score += 2;

    score += rel_w;

    if (history_count > 6)
        score += 1;

    if (hour >= 0 && (hour >= 23 || hour <= 4) && emotion > 0)
        score += 1;

    return score;
}

static void apply_tier_to_selection(hu_model_selection_t *sel, const hu_model_router_config_t *cfg,
                                    int score) {
    if (score <= 0) {
        sel->tier = HU_TIER_REFLEXIVE;
        if (cfg->on_device_available && cfg->on_device_model && cfg->on_device_model_len > 0) {
            sel->model = cfg->on_device_model;
            sel->model_len = cfg->on_device_model_len;
        } else {
            sel->model = cfg->reflexive_model;
            sel->model_len = cfg->reflexive_model_len;
        }
        sel->thinking_budget = 0;
        sel->temperature = 0.6;
    } else if (score <= 3) {
        sel->tier = HU_TIER_CONVERSATIONAL;
        sel->model = cfg->conversational_model;
        sel->model_len = cfg->conversational_model_len;
        sel->thinking_budget = 1024;
        sel->temperature = 0.7;
    } else if (score <= 6) {
        sel->tier = HU_TIER_ANALYTICAL;
        sel->model = cfg->analytical_model;
        sel->model_len = cfg->analytical_model_len;
        sel->thinking_budget = 4096;
        sel->temperature = 0.7;
    } else {
        sel->tier = HU_TIER_DEEP;
        sel->model = cfg->deep_model;
        sel->model_len = cfg->deep_model_len;
        sel->thinking_budget = 8192;
        sel->temperature = 0.6;
    }
}

static void apply_tier_override(hu_model_selection_t *sel, const hu_model_router_config_t *cfg,
                                hu_cognitive_tier_t tier) {
    switch (tier) {
    case HU_TIER_REFLEXIVE:
        sel->tier = HU_TIER_REFLEXIVE;
        if (cfg->on_device_available && cfg->on_device_model && cfg->on_device_model_len > 0) {
            sel->model = cfg->on_device_model;
            sel->model_len = cfg->on_device_model_len;
        } else {
            sel->model = cfg->reflexive_model;
            sel->model_len = cfg->reflexive_model_len;
        }
        sel->thinking_budget = 0;
        sel->temperature = 0.9;
        break;
    case HU_TIER_CONVERSATIONAL:
        sel->tier = HU_TIER_CONVERSATIONAL;
        sel->model = cfg->conversational_model;
        sel->model_len = cfg->conversational_model_len;
        sel->thinking_budget = 1024;
        sel->temperature = 0.8;
        break;
    case HU_TIER_ANALYTICAL:
        sel->tier = HU_TIER_ANALYTICAL;
        sel->model = cfg->analytical_model;
        sel->model_len = cfg->analytical_model_len;
        sel->thinking_budget = 4096;
        sel->temperature = 0.7;
        break;
    case HU_TIER_DEEP:
        sel->tier = HU_TIER_DEEP;
        sel->model = cfg->deep_model;
        sel->model_len = cfg->deep_model_len;
        sel->thinking_budget = 8192;
        sel->temperature = 0.6;
        break;
    }
}

hu_model_selection_t hu_model_route(const hu_model_router_config_t *cfg,
                                    const char *msg, size_t msg_len,
                                    const char *relationship, size_t relationship_len,
                                    int hour, size_t history_count) {
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.source = HU_ROUTE_HEURISTIC;

    if (!cfg || !msg || msg_len == 0) {
        sel.model = cfg ? cfg->conversational_model : "gemini-3-flash-preview";
        sel.model_len = cfg ? cfg->conversational_model_len : 22;
        sel.tier = HU_TIER_CONVERSATIONAL;
        return sel;
    }

    int score = compute_heuristic_score(msg, msg_len, relationship, relationship_len, hour,
                                        history_count);
    apply_tier_to_selection(&sel, cfg, score);

    if (cfg->conversation_floor > sel.tier)
        apply_tier_override(&sel, cfg, cfg->conversation_floor);

    /* Record to global decision log for dashboard visibility */
    hu_route_log_record(hu_route_global_log(), &sel, score, (int64_t)time(NULL));

    return sel;
}

hu_model_selection_t hu_model_route_with_judge(const hu_model_router_config_t *cfg,
                                               const char *msg, size_t msg_len,
                                               const char *relationship, size_t relationship_len,
                                               int hour, size_t history_count,
                                               hu_provider_t *judge_provider,
                                               const char *judge_model, size_t judge_model_len,
                                               hu_allocator_t *alloc,
                                               hu_route_cache_t *cache) {
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    if (!cfg || !msg || msg_len == 0 || !judge_provider || !alloc) {
        return hu_model_route(cfg, msg, msg_len, relationship, relationship_len, hour,
                              history_count);
    }

    int score = compute_heuristic_score(msg, msg_len, relationship, relationship_len, hour,
                                        history_count);

    int64_t now = (int64_t)time(NULL);

    /* Check cache first */
    hu_cognitive_tier_t cached_tier;
    if (cache && hu_route_cache_get(cache, msg, msg_len, now, &cached_tier)) {
        sel.source = HU_ROUTE_JUDGE_CACHED;
        apply_tier_override(&sel, cfg, cached_tier);
        if (cfg->conversation_floor > sel.tier)
            apply_tier_override(&sel, cfg, cfg->conversation_floor);
        hu_route_log_record(hu_route_global_log(), &sel, score, now);
        return sel;
    }

    /* Call the judge provider */
#ifdef HU_IS_TEST
    (void)judge_model;
    (void)judge_model_len;
    /* In test mode, skip the actual LLM call and fall through to heuristics */
    sel.source = HU_ROUTE_JUDGE_FALLBACK;
    apply_tier_to_selection(&sel, cfg, score);
    if (cfg->conversation_floor > sel.tier)
        apply_tier_override(&sel, cfg, cfg->conversation_floor);
    hu_route_log_record(hu_route_global_log(), &sel, score, now);
    return sel;
#else
    if (!judge_provider->vtable || !judge_provider->vtable->chat_with_system) {
        sel.source = HU_ROUTE_JUDGE_FALLBACK;
        apply_tier_to_selection(&sel, cfg, score);
        if (cfg->conversation_floor > sel.tier)
            apply_tier_override(&sel, cfg, cfg->conversation_floor);
        hu_route_log_record(hu_route_global_log(), &sel, score, now);
        return sel;
    }

    const char *system_prompt = hu_route_judge_system_prompt();
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = judge_provider->vtable->chat_with_system(
        judge_provider->ctx, alloc, system_prompt, strlen(system_prompt),
        msg, msg_len, judge_model, judge_model_len, 0.0, &response, &response_len);

    if (err != HU_OK || !response || response_len == 0) {
        if (response)
            alloc->free(alloc->ctx, response, response_len + 1);
        sel.source = HU_ROUTE_JUDGE_FALLBACK;
        apply_tier_to_selection(&sel, cfg, score);
        if (cfg->conversation_floor > sel.tier)
            apply_tier_override(&sel, cfg, cfg->conversation_floor);
        hu_route_log_record(hu_route_global_log(), &sel, score, now);
        return sel;
    }

    hu_cognitive_tier_t judge_tier;
    if (hu_route_parse_judge_response(response, response_len, &judge_tier)) {
        sel.source = HU_ROUTE_JUDGE;
        apply_tier_override(&sel, cfg, judge_tier);
        if (cache)
            hu_route_cache_put(cache, msg, msg_len, now, judge_tier);
    } else {
        sel.source = HU_ROUTE_JUDGE_FALLBACK;
        apply_tier_to_selection(&sel, cfg, score);
    }

    if (cfg->conversation_floor > sel.tier)
        apply_tier_override(&sel, cfg, cfg->conversation_floor);

    alloc->free(alloc->ctx, response, response_len + 1);
    hu_route_log_record(hu_route_global_log(), &sel, score, now);
    return sel;
#endif
}

/* ── FNV-1a prompt hash for cache keys ────────────────────────────────── */

uint64_t hu_route_hash_prompt(const char *msg, size_t msg_len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < msg_len; i++) {
        hash ^= (uint64_t)(unsigned char)msg[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* ── Route cache ──────────────────────────────────────────────────────── */

void hu_route_cache_init(hu_route_cache_t *cache) {
    if (!cache)
        return;
    memset(cache, 0, sizeof(*cache));
}

bool hu_route_cache_get(hu_route_cache_t *cache, const char *msg, size_t msg_len,
                        int64_t now_secs, hu_cognitive_tier_t *tier) {
    if (!cache || !msg || msg_len == 0 || !tier)
        return false;
    uint64_t hash = hu_route_hash_prompt(msg, msg_len);
    size_t start = (size_t)(hash % HU_ROUTE_CACHE_SIZE);
    for (size_t probe = 0; probe < 4; probe++) {
        size_t idx = (start + probe) % HU_ROUTE_CACHE_SIZE;
        hu_route_cache_entry_t *e = &cache->entries[idx];
        if (!e->occupied)
            return false;
        if (e->hash == hash && e->msg_len == msg_len &&
            (now_secs - e->timestamp) < HU_ROUTE_CACHE_TTL_SECS) {
            *tier = e->tier;
            return true;
        }
    }
    return false;
}

void hu_route_cache_put(hu_route_cache_t *cache, const char *msg, size_t msg_len,
                        int64_t now_secs, hu_cognitive_tier_t tier) {
    if (!cache || !msg || msg_len == 0)
        return;
    uint64_t hash = hu_route_hash_prompt(msg, msg_len);
    size_t start = (size_t)(hash % HU_ROUTE_CACHE_SIZE);
    size_t best = start;
    for (size_t probe = 0; probe < 4; probe++) {
        size_t idx = (start + probe) % HU_ROUTE_CACHE_SIZE;
        hu_route_cache_entry_t *e = &cache->entries[idx];
        if (!e->occupied || (now_secs - e->timestamp) >= HU_ROUTE_CACHE_TTL_SECS) {
            best = idx;
            break;
        }
        if (e->hash == hash && e->msg_len == msg_len) {
            best = idx;
            break;
        }
    }
    hu_route_cache_entry_t *e = &cache->entries[best];
    e->hash = hash;
    e->msg_len = msg_len;
    e->tier = tier;
    e->timestamp = now_secs;
    e->occupied = true;
}

/* ── Judge response parser ────────────────────────────────────────────── */

static bool ci_match(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool match_tier_value(const char *val, size_t val_len, hu_cognitive_tier_t *tier) {
    if (ci_match(val, val_len, "REFLEXIVE") || ci_match(val, val_len, "SIMPLE")) {
        *tier = HU_TIER_REFLEXIVE;
        return true;
    }
    if (ci_match(val, val_len, "CONVERSATIONAL") || ci_match(val, val_len, "MEDIUM")) {
        *tier = HU_TIER_CONVERSATIONAL;
        return true;
    }
    if (ci_match(val, val_len, "ANALYTICAL") || ci_match(val, val_len, "COMPLEX") ||
        ci_match(val, val_len, "RESEARCH")) {
        *tier = HU_TIER_ANALYTICAL;
        return true;
    }
    if (ci_match(val, val_len, "DEEP") || ci_match(val, val_len, "REASONING")) {
        *tier = HU_TIER_DEEP;
        return true;
    }
    return false;
}

bool hu_route_parse_judge_response(const char *response, size_t response_len,
                                   hu_cognitive_tier_t *tier) {
    if (!response || response_len == 0 || !tier)
        return false;

    const char *end = response + response_len;

    /* Scan for "tier" key in JSON (tolerant of whitespace, newlines, single quotes) */
    for (const char *p = response; p < end - 4; p++) {
        if (p > response && (p[-1] == '"' || p[-1] == '\'') &&
            (p[0] == 't' || p[0] == 'T') && (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'e' || p[2] == 'E') && (p[3] == 'r' || p[3] == 'R')) {
            const char *after_key = p + 4;
            if (after_key < end && (*after_key == '"' || *after_key == '\''))
                after_key++;
            while (after_key < end && (*after_key == ' ' || *after_key == '\t' ||
                                       *after_key == '\n' || *after_key == '\r' ||
                                       *after_key == ':'))
                after_key++;
            if (after_key < end && (*after_key == '"' || *after_key == '\'')) {
                char quote = *after_key;
                after_key++;
                const char *val_start = after_key;
                while (after_key < end && *after_key != quote)
                    after_key++;
                size_t val_len = (size_t)(after_key - val_start);
                if (match_tier_value(val_start, val_len, tier))
                    return true;
            }
        }
    }

    /* Fallback: scan for bare tier names (handles malformed responses) */
    static const char *bare_names[] = {
        "REFLEXIVE", "CONVERSATIONAL", "ANALYTICAL", "DEEP",
        "SIMPLE",    "MEDIUM",         "COMPLEX",    "RESEARCH", "REASONING",
    };
    for (size_t i = 0; i < sizeof(bare_names) / sizeof(bare_names[0]); i++) {
        size_t nlen = strlen(bare_names[i]);
        for (const char *p = response; p + nlen <= end; p++) {
            if (ci_match(p, nlen, bare_names[i])) {
                bool left_ok = (p == response || !isalpha((unsigned char)p[-1]));
                bool right_ok = (p + nlen >= end || !isalpha((unsigned char)p[nlen]));
                if (left_ok && right_ok)
                    return match_tier_value(p, nlen, tier);
            }
        }
    }
    return false;
}

/* ── Judge system prompt (adapted from EdgeClaw token-saver-judge.md) ── */

static const char JUDGE_SYSTEM_PROMPT[] =
    "You are a task complexity classifier for an AI assistant. "
    "Classify each task into exactly one of four tiers based on the nature of the work.\n"
    "\n"
    "## Tiers\n"
    "\n"
    "REFLEXIVE - Pure acknowledgment or backchannel. Greetings, single-word replies, "
    "simple confirmations, emoji-only messages.\n"
    "\n"
    "CONVERSATIONAL - Standard assistant work. Writing emails, simple Q&A, factual lookups, "
    "translation, formatting, code snippets, scheduling, reminders.\n"
    "\n"
    "ANALYTICAL - Multi-step reasoning or emotional depth. Advice-seeking, pros/cons analysis, "
    "code review, debugging, emotional support, relationship discussions, financial planning.\n"
    "\n"
    "DEEP - Complex synthesis or crisis. Multi-source research, system design, mathematical proofs, "
    "life decisions, mental health crisis, grief support, legal/medical analysis.\n"
    "\n"
    "## Disambiguation\n"
    "\n"
    "- Short greetings (< 5 words, no question) -> REFLEXIVE\n"
    "- Data analysis, single-file editing -> CONVERSATIONAL\n"
    "- Emotional keywords (scared, struggling, overwhelmed) -> ANALYTICAL minimum\n"
    "- Crisis keywords (suicidal, dying, emergency) -> DEEP\n"
    "- When unsure, choose CONVERSATIONAL\n"
    "\n"
    "CRITICAL: Output ONLY a raw JSON object. No markdown, no explanation.\n"
    "{\"tier\":\"REFLEXIVE|CONVERSATIONAL|ANALYTICAL|DEEP\"}";

const char *hu_route_judge_system_prompt(void) {
    return JUDGE_SYSTEM_PROMPT;
}

/* Global routing log static state (must appear before hu_route_log_* uses it). */
static hu_route_decision_log_t s_global_log;
static bool s_global_log_initialized = false;
static pthread_mutex_t s_global_log_mutex;
static pthread_once_t s_global_log_mutex_once = PTHREAD_ONCE_INIT;

static void hu_route_global_log_mutex_init_once(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_global_log_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void hu_route_ensure_global_log_mutex(void) {
    pthread_once(&s_global_log_mutex_once, hu_route_global_log_mutex_init_once);
}

/* ── Routing decision log ─────────────────────────────────────────────── */

void hu_route_log_init(hu_route_decision_log_t *log) {
    if (!log)
        return;
    memset(log, 0, sizeof(*log));
}

void hu_route_log_record(hu_route_decision_log_t *log, const hu_model_selection_t *sel,
                         int heuristic_score, int64_t timestamp) {
    if (!log || !sel)
        return;
    bool global = (log == &s_global_log);
    if (global) {
        hu_route_ensure_global_log_mutex();
        pthread_mutex_lock(&s_global_log_mutex);
    }
    hu_route_decision_t *entry = &log->entries[log->head];
    entry->tier = sel->tier;
    entry->source = sel->source;
    entry->timestamp = timestamp;
    entry->heuristic_score = heuristic_score;
    if (sel->model && sel->model_len > 0) {
        size_t copy_len = sel->model_len < 63 ? sel->model_len : 63;
        memcpy(entry->model, sel->model, copy_len);
        entry->model[copy_len] = '\0';
    } else {
        entry->model[0] = '\0';
    }
    log->head = (log->head + 1) % HU_ROUTE_LOG_SIZE;
    if (log->count < HU_ROUTE_LOG_SIZE)
        log->count++;
    if (global)
        pthread_mutex_unlock(&s_global_log_mutex);
}

size_t hu_route_log_count(const hu_route_decision_log_t *log) {
    if (!log)
        return 0;
    bool global = (log == &s_global_log);
    if (global) {
        hu_route_ensure_global_log_mutex();
        pthread_mutex_lock(&s_global_log_mutex);
    }
    size_t c = log->count;
    if (global)
        pthread_mutex_unlock(&s_global_log_mutex);
    return c;
}

const hu_route_decision_t *hu_route_log_get(const hu_route_decision_log_t *log, size_t index) {
    if (!log || index >= log->count)
        return NULL;
    bool global = (log == &s_global_log);
    if (global) {
        hu_route_ensure_global_log_mutex();
        pthread_mutex_lock(&s_global_log_mutex);
    }
    /* Entries are stored newest-at-head. Index 0 = oldest visible entry. */
    size_t start;
    if (log->count < HU_ROUTE_LOG_SIZE)
        start = 0;
    else
        start = log->head; /* oldest is at head (about to be overwritten) */
    size_t actual = (start + index) % HU_ROUTE_LOG_SIZE;
    const hu_route_decision_t *out = &log->entries[actual];
    if (global)
        pthread_mutex_unlock(&s_global_log_mutex);
    return out;
}

void hu_route_log_tier_counts(const hu_route_decision_log_t *log, size_t counts[4]) {
    memset(counts, 0, sizeof(size_t) * 4);
    if (!log)
        return;
    bool global = (log == &s_global_log);
    if (global) {
        hu_route_ensure_global_log_mutex();
        pthread_mutex_lock(&s_global_log_mutex);
    }
    size_t start = (log->count < HU_ROUTE_LOG_SIZE) ? 0 : log->head;
    for (size_t i = 0; i < log->count; i++) {
        const hu_route_decision_t *d = &log->entries[(start + i) % HU_ROUTE_LOG_SIZE];
        if (d->tier <= HU_TIER_DEEP)
            counts[d->tier]++;
    }
    if (global)
        pthread_mutex_unlock(&s_global_log_mutex);
}

/* ── String conversions ───────────────────────────────────────────────── */

const char *hu_cognitive_tier_str(hu_cognitive_tier_t tier) {
    switch (tier) {
    case HU_TIER_REFLEXIVE:
        return "reflexive";
    case HU_TIER_CONVERSATIONAL:
        return "conversational";
    case HU_TIER_ANALYTICAL:
        return "analytical";
    case HU_TIER_DEEP:
        return "deep";
    default:
        return "unknown";
    }
}

const char *hu_route_source_str(hu_route_source_t source) {
    switch (source) {
    case HU_ROUTE_HEURISTIC:
        return "heuristic";
    case HU_ROUTE_JUDGE:
        return "judge";
    case HU_ROUTE_JUDGE_CACHED:
        return "judge_cached";
    case HU_ROUTE_JUDGE_FALLBACK:
        return "judge_fallback";
    default:
        return "unknown";
    }
}

/* ── Global decision log ──────────────────────────────────────────────── */

hu_route_decision_log_t *hu_route_global_log(void) {
    hu_route_ensure_global_log_mutex();
    pthread_mutex_lock(&s_global_log_mutex);
    if (!s_global_log_initialized) {
        hu_route_log_init(&s_global_log);
        s_global_log_initialized = true;
    }
    pthread_mutex_unlock(&s_global_log_mutex);
    return &s_global_log;
}

void hu_route_global_log_lock(void) {
    hu_route_ensure_global_log_mutex();
    pthread_mutex_lock(&s_global_log_mutex);
}

void hu_route_global_log_unlock(void) {
    pthread_mutex_unlock(&s_global_log_mutex);
}
