/* src/daemon_reaction_poll.c
 *
 * CF-3 closure: production iMessage reaction polling for the daemon.
 * `hu_daemon_reaction_poll_tick` is the simple since-watermark entry;
 * `hu_daemon_tick_reaction_poll` adds poll_interval_seconds gating for
 * the daemon main loop.
 */

#include "human/daemon_reaction_poll.h"

#include "human/agent/reaction_handler.h"
#include "human/channels/imessage_reactions.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/ml/dpo.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per ~/.claude/rules/silent-config-gated-subsystems.md: emit ONE
 * operator-visible log line per process when reaction polling is
 * disabled or enabled, so the operator can tell "we silently skipped"
 * apart from "we ran". Guards are process-scoped via atomic_bool. */
static atomic_bool g_warned_reaction_poll_disabled_cfg = false;
static atomic_bool g_warned_reaction_poll_enabled_cfg = false;
static atomic_bool g_warned_reaction_poll_disabled_sub = false;
static atomic_bool g_warned_reaction_poll_enabled_sub = false;

#if HU_IS_TEST
void hu_daemon_reaction_poll_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_reaction_poll_disabled_cfg, false);
    atomic_store(&g_warned_reaction_poll_enabled_cfg, false);
    atomic_store(&g_warned_reaction_poll_disabled_sub, false);
    atomic_store(&g_warned_reaction_poll_enabled_sub, false);
}
#endif

void hu_daemon_reaction_wire_collector(struct hu_dpo_collector *collector) {
    hu_reaction_handler_set_collector((hu_dpo_collector_t *)collector);
}

void hu_daemon_reaction_wire_personal_model(struct hu_personal_model *model) {
    hu_reaction_handler_set_personal_model(model);
}

#if HU_IS_TEST
static int *g_imessage_poll_call_counter = NULL;
static int g_poll_count_for_test = 0;

void hu_daemon_set_poll_call_counter_for_test(int *counter) {
    g_imessage_poll_call_counter = counter;
}

int hu_daemon_reaction_poll_get_count_for_test(void) {
    return g_poll_count_for_test;
}

void hu_daemon_reaction_poll_reset_count_for_test(void) {
    g_poll_count_for_test = 0;
}
#endif

static bool reaction_collection_wants_imessage_cfg(const hu_config_t *cfg) {
    if (!cfg || !cfg->reaction_collection.enabled)
        return false;
    if (cfg->reaction_collection.channel_count == 0)
        return true;
    for (size_t i = 0; i < cfg->reaction_collection.channel_count; i++) {
        if (strcmp(cfg->reaction_collection.channels[i], "imessage") == 0)
            return true;
    }
    return false;
}

static bool reaction_collection_wants_imessage_sub(const hu_reaction_collection_config_t *cfg) {
    if (!cfg || !cfg->enabled)
        return false;
    if (cfg->channel_count == 0)
        return true;
    for (size_t i = 0; i < cfg->channel_count; i++) {
        if (strcmp(cfg->channels[i], "imessage") == 0)
            return true;
    }
    return false;
}

static void free_event_strings(hu_reaction_event_t *ev) {
    free((void *)ev->target_thread_id);
    free((void *)ev->target_message_ref);
    free((void *)ev->sender_handle);
    free((void *)ev->emoji);
    ev->target_thread_id = NULL;
    ev->target_message_ref = NULL;
    ev->sender_handle = NULL;
    ev->emoji = NULL;
}

static const char *resolve_chatdb_path(const hu_reaction_collection_config_t *cfg) {
    if (cfg && cfg->chatdb_path[0])
        return cfg->chatdb_path;
    const char *env = getenv("HU_CHATDB");
    if (env && env[0])
        return env;
    static char home_path[512];
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    snprintf(home_path, sizeof(home_path), "%s/Library/Messages/chat.db", home);
    return home_path;
}

hu_error_t hu_daemon_reaction_poll_tick(const hu_config_t *cfg, int64_t since_unix,
                                        size_t *out_ingested) {
    if (out_ingested)
        *out_ingested = 0;
    if (!cfg) {
        hu_log_warn_once(&g_warned_reaction_poll_disabled_cfg, "daemon", NULL,
                         "reaction_collection poll skipped: cfg is NULL — caller "
                         "did not pass a daemon config. This is a programmer error, "
                         "not an operator misconfiguration.");
        return HU_OK;
    }
    if (!reaction_collection_wants_imessage_cfg(cfg)) {
        hu_log_info_once(&g_warned_reaction_poll_disabled_cfg, "daemon", NULL,
                         "reaction_collection (imessage) disabled by config "
                         "(cfg->reaction_collection.enabled=false or no 'imessage' "
                         "in reaction_collection.channels) — set "
                         "reaction_collection.enabled=true in config.json to activate");
        return HU_OK;
    }
    hu_log_info_once(&g_warned_reaction_poll_enabled_cfg, "daemon", NULL,
                     "reaction_collection (imessage) enabled — polling chat.db for tapbacks");

    const char *db = getenv("HU_CHATDB");
    if (!db || !db[0])
        return HU_OK;

    hu_reaction_event_t events[16];
    size_t n = 0;
    hu_error_t pe = hu_imessage_poll_reactions(db, since_unix, events, 16, &n);

#if HU_IS_TEST
    if (g_imessage_poll_call_counter)
        (*g_imessage_poll_call_counter)++;
#endif

    if (pe != HU_OK && pe != HU_ERR_NOT_SUPPORTED) {
        for (size_t i = 0; i < n; i++)
            free_event_strings(&events[i]);
        return pe;
    }

    size_t ingested = 0;
    for (size_t i = 0; i < n; i++) {
        if (events[i].channel_id == NULL)
            events[i].channel_id = "imessage";
        hu_error_t he = hu_reaction_handler_handle_event(&events[i]);
        if (he == HU_OK)
            ingested++;
        free_event_strings(&events[i]);
    }
    if (out_ingested)
        *out_ingested = ingested;
    return HU_OK;
}

hu_error_t hu_daemon_tick_reaction_poll(const hu_reaction_collection_config_t *cfg,
                                        int64_t now_unix, int64_t *last_poll_unix_inout,
                                        int64_t *watermark_inout) {
    if (!cfg || !last_poll_unix_inout || !watermark_inout)
        return HU_ERR_INVALID_ARGUMENT;
    if (!reaction_collection_wants_imessage_sub(cfg)) {
        hu_log_info_once(&g_warned_reaction_poll_disabled_sub, "daemon", NULL,
                         "reaction_collection (imessage) disabled by config "
                         "(reaction_collection.enabled=false or 'imessage' not in "
                         "reaction_collection.channels) — set "
                         "reaction_collection.enabled=true in config.json to activate");
        return HU_OK;
    }
    hu_log_info_once(&g_warned_reaction_poll_enabled_sub, "daemon", NULL,
                     "reaction_collection (imessage) enabled — polling chat.db for tapbacks");

    int interval = cfg->poll_interval_seconds > 0 ? cfg->poll_interval_seconds : 30;
    if (*last_poll_unix_inout > 0 && now_unix - *last_poll_unix_inout < interval)
        return HU_OK;

    const char *db = resolve_chatdb_path(cfg);
    if (!db || !db[0])
        return HU_OK;

    hu_reaction_event_t events[8];
    size_t n = 0;
    int64_t since = *watermark_inout > 0 ? *watermark_inout : now_unix;
    hu_error_t err = hu_imessage_poll_reactions(db, since, events, 8, &n);

#if HU_IS_TEST
    g_poll_count_for_test++;
    if (g_imessage_poll_call_counter)
        (*g_imessage_poll_call_counter)++;
#endif

    *last_poll_unix_inout = now_unix;
    *watermark_inout = now_unix;

    if (err == HU_OK) {
        for (size_t i = 0; i < n; i++)
            (void)hu_reaction_handler_handle_event(&events[i]);
    }
    for (size_t i = 0; i < n; i++)
        free_event_strings(&events[i]);

    return err == HU_ERR_IO ? err : HU_OK;
}

#if HU_IS_TEST
hu_error_t hu_daemon_tick_for_test(const hu_config_t *cfg) {
    if (!cfg)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_daemon_reaction_poll_tick(cfg, 0, NULL);
}
#endif
