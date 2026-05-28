#ifndef HU_MODEL_ROUTER_H
#define HU_MODEL_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_cognitive_tier {
    HU_TIER_REFLEXIVE = 0,  /* backchannel, ack, simple reply */
    HU_TIER_CONVERSATIONAL, /* standard chat, moderate complexity */
    HU_TIER_ANALYTICAL,     /* questions, emotional depth, advice */
    HU_TIER_DEEP            /* complex reasoning, crisis, life decisions */
} hu_cognitive_tier_t;

typedef enum hu_route_source {
    HU_ROUTE_HEURISTIC = 0, /* keyword/score-based fast path */
    HU_ROUTE_JUDGE,         /* LLM-as-Judge classification */
    HU_ROUTE_JUDGE_CACHED,  /* LLM judge result from cache */
    HU_ROUTE_JUDGE_FALLBACK /* judge failed, fell back to heuristic */
} hu_route_source_t;

typedef struct hu_model_selection {
    const char *model;
    size_t model_len;
    int thinking_budget; /* 0 = none, >0 = token budget for reasoning */
    double temperature;  /* 0.0 = use default, >0 = override */
    hu_cognitive_tier_t tier;
    hu_route_source_t source;
} hu_model_selection_t;

typedef struct hu_model_router_config {
    const char *reflexive_model; /* fast: backchannel, ack */
    size_t reflexive_model_len;
    const char *conversational_model; /* standard: normal chat */
    size_t conversational_model_len;
    const char *analytical_model; /* capable: emotional, questions */
    size_t analytical_model_len;
    const char *deep_model; /* most capable: complex reasoning */
    size_t deep_model_len;
    const char *on_device_model; /* on-device fallback (e.g. apple-foundationmodel) */
    size_t on_device_model_len;
    bool on_device_available;               /* set true when an on-device provider is probed;
                                             * defaults to false — callers must set explicitly */
    bool force_on_device;                   /* when true, on_device_model is preferred at ALL
                                             * tiers (not just REFLEXIVE), as long as
                                             * on_device_available is also true. Enables the
                                             * "all contacts through local MLX for personalized
                                             * voice" policy. Default false preserves the
                                             * tier-graduated behavior (REFLEXIVE only). */
    hu_cognitive_tier_t conversation_floor; /* minimum tier for conversational channels;
                                             * 0 (HU_TIER_REFLEXIVE) = no floor (default) */
    /* Seth-voice mlx_local LoRA routing (2026-05-27 Dermot humanness recovery,
     * C2). Distinct from on_device_model (the tiny apple-foundationmodel): this
     * names the locally-served 31B LoRA adapter (e.g. via mlx-server.py) that
     * carries the personalized voice. When mlx_local_enabled AND
     * mlx_local_healthy are both true, REFLEXIVE and CONVERSATIONAL tiers route
     * to mlx_local_model; ANALYTICAL and DEEP stay on the cloud model (local
     * 31B trades base reasoning for voice — see lora-scale-default-or-die). The
     * router is a pure function: the caller probes the server (hu_mlx_local_probe)
     * and sets mlx_local_healthy before routing, mirroring on_device_available. */
    bool mlx_local_enabled;      /* operator opt-in; default false */
    bool mlx_local_healthy;      /* caller-set probe result; false = fall back to cloud */
    const char *mlx_local_model; /* local adapter identifier the mlx provider knows */
    size_t mlx_local_model_len;
} hu_model_router_config_t;

/* Analyze message content and context to select the optimal model + thinking budget.
 * relationship: "family", "close_friend", "friend", "regular", etc. NULL = unknown.
 * hour: 0-23, current hour of day. */
hu_model_selection_t hu_model_route(const hu_model_router_config_t *cfg, const char *msg,
                                    size_t msg_len, const char *relationship,
                                    size_t relationship_len, int hour, size_t history_count);

/* Initialize config with sensible Gemini defaults */
hu_model_router_config_t hu_model_router_default_config(void);

/* Check whether the on-device model is sufficient for a given tier.
 * Currently returns true only for REFLEXIVE (short acks, simple greetings).
 * On-device context window (~4096 tokens) limits complex interactions. */
bool hu_model_router_on_device_suitable(hu_cognitive_tier_t tier);

/* Forward declarations for judge call */
struct hu_provider;
struct hu_allocator;

/* ── LLM-as-Judge cost router (inspired by EdgeClaw ClawXRouter) ──────── */

#define HU_ROUTE_CACHE_SIZE     64
#define HU_ROUTE_CACHE_TTL_SECS 300 /* 5 minutes */

typedef struct hu_route_cache_entry {
    uint64_t hash;
    size_t msg_len;
    hu_cognitive_tier_t tier;
    int64_t timestamp;
    bool occupied;
} hu_route_cache_entry_t;

typedef struct hu_route_cache {
    hu_route_cache_entry_t entries[HU_ROUTE_CACHE_SIZE];
} hu_route_cache_t;

void hu_route_cache_init(hu_route_cache_t *cache);

/* Lookup a cached tier. Returns true and fills *tier if found and not expired. */
bool hu_route_cache_get(hu_route_cache_t *cache, const char *msg, size_t msg_len, int64_t now_secs,
                        hu_cognitive_tier_t *tier);

void hu_route_cache_put(hu_route_cache_t *cache, const char *msg, size_t msg_len, int64_t now_secs,
                        hu_cognitive_tier_t tier);

/* Parse an LLM judge response into a cognitive tier.
 * Expects JSON like {"tier":"REFLEXIVE"} in the response text.
 * Returns true on success, false on parse failure. */
bool hu_route_parse_judge_response(const char *response, size_t response_len,
                                   hu_cognitive_tier_t *tier);

/* The judge system prompt for tier classification. */
const char *hu_route_judge_system_prompt(void);

/* FNV-1a hash for prompt caching */
uint64_t hu_route_hash_prompt(const char *msg, size_t msg_len);

/* Route with an optional LLM judge. Calls judge_provider with the classification
 * system prompt, caches the result, and falls back to heuristics on failure.
 * Pass NULL for judge_provider to behave identically to hu_model_route. */
hu_model_selection_t hu_model_route_with_judge(const hu_model_router_config_t *cfg, const char *msg,
                                               size_t msg_len, const char *relationship,
                                               size_t relationship_len, int hour,
                                               size_t history_count,
                                               struct hu_provider *judge_provider,
                                               const char *judge_model, size_t judge_model_len,
                                               struct hu_allocator *alloc, hu_route_cache_t *cache);

/* ── Routing decision log (ring buffer) ───────────────────────────────── */

#define HU_ROUTE_LOG_SIZE 100

typedef struct hu_route_decision {
    hu_cognitive_tier_t tier;
    hu_route_source_t source;
    int64_t timestamp;
    int heuristic_score;
    char model[64];
} hu_route_decision_t;

typedef struct hu_route_decision_log {
    hu_route_decision_t entries[HU_ROUTE_LOG_SIZE];
    size_t head;
    size_t count;
} hu_route_decision_log_t;

void hu_route_log_init(hu_route_decision_log_t *log);
void hu_route_log_record(hu_route_decision_log_t *log, const hu_model_selection_t *sel,
                         int heuristic_score, int64_t timestamp);
size_t hu_route_log_count(const hu_route_decision_log_t *log);
const hu_route_decision_t *hu_route_log_get(const hu_route_decision_log_t *log, size_t index);

/* Tier distribution: counts per tier across logged decisions */
void hu_route_log_tier_counts(const hu_route_decision_log_t *log, size_t counts[4]);

const char *hu_cognitive_tier_str(hu_cognitive_tier_t tier);
const char *hu_route_source_str(hu_route_source_t source);

/* Global decision log singleton — threadsafe reads from gateway.
 * For thread-safe iteration, bracket reads with lock/unlock. */
hu_route_decision_log_t *hu_route_global_log(void);
void hu_route_global_log_lock(void);
void hu_route_global_log_unlock(void);

#endif
