/* Exercises src/daemon/daemon_job_hold.c: the HOLD/ONE_OFF predicate, and the
 * failed-turn hook under HU_JOB_HOLD off|shadow|live (design §5, §7). The
 * model probe is forced with hu_mlx_admin_set_test_health; the channel is a
 * mock whose vtable name is "imessage". Hermetic: :memory: db, no network. */
#include "human/agent.h"
#include "human/daemon/job_hold.h"
#include "test_framework.h"

static void job_hold_decide_truth_table(void) {
    const hu_error_t transport[] = {HU_ERR_IO, HU_ERR_TIMEOUT, HU_ERR_PROVIDER_UNAVAILABLE};
    for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
        HU_ASSERT_TRUE(hu_agent_error_is_transport(transport[i]));
        HU_ASSERT_EQ(hu_job_hold_decide(transport[i], HU_JOB_PROBE_DOWN), HU_JOB_TURN_HOLD);
        HU_ASSERT_EQ(hu_job_hold_decide(transport[i], HU_JOB_PROBE_UP), HU_JOB_TURN_ONE_OFF);
        HU_ASSERT_EQ(hu_job_hold_decide(transport[i], HU_JOB_PROBE_UNKNOWN), HU_JOB_TURN_ONE_OFF);
    }
    /* The round trip completed (or never failed): the model answered. */
    const hu_error_t other[] = {HU_OK, HU_ERR_PROVIDER_RESPONSE, HU_ERR_PROVIDER_AUTH,
                                HU_ERR_INVALID_ARGUMENT};
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++) {
        HU_ASSERT_FALSE(hu_agent_error_is_transport(other[i]));
        HU_ASSERT_EQ(hu_job_hold_decide(other[i], HU_JOB_PROBE_DOWN), HU_JOB_TURN_ONE_OFF);
    }
}

#ifdef HU_ENABLE_SQLITE
#include "human/channel.h"
#include "human/config.h"
#include "human/daemon.h"
#include "human/daemon/job_queue.h"
#include "human/daemon/owner_notify.h"
#include "human/memory.h"
#include "human/memory/job_queue_repo.h"
#include "human/ml/mlx_admin.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *jh_name_imessage(void *ctx) {
    (void)ctx;
    return "imessage";
}
static const char *jh_name_telegram(void *ctx) {
    (void)ctx;
    return "telegram";
}

static const hu_channel_vtable_t jh_vt_imessage = {.name = jh_name_imessage};
static const hu_channel_vtable_t jh_vt_telegram = {.name = jh_name_telegram};

typedef struct jh_fixture {
    char *saved_queue;
    char *saved_hold;
    hu_allocator_t alloc; /* the memory keeps a pointer to it */
    hu_memory_t mem;
    sqlite3 *db;
    hu_channel_t channel;
    hu_service_channel_t ch;
    hu_provider_entry_t provider;
    hu_config_t config;
    hu_channel_loop_msg_t msgs[4];
} jh_fixture_t;

static char *jh_dup_env(const char *name) {
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

static void jh_put_env(const char *name, char *saved) {
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

static void jh_msg(hu_channel_loop_msg_t *m, const char *who, const char *chat, int64_t rowid,
                   const char *text) {
    memset(m, 0, sizeof(*m));
    snprintf(m->session_key, sizeof(m->session_key), "%s", who);
    snprintf(m->chat_id, sizeof(m->chat_id), "%s", chat);
    snprintf(m->content, sizeof(m->content), "%s", text);
    snprintf(m->guid, sizeof(m->guid), "GUID-%lld", (long long)rowid);
    m->message_id = rowid;
    m->timestamp_sec = 1790000000 + rowid;
}

/* queue / hold: values for HU_JOB_QUEUE / HU_JOB_HOLD (NULL = unset). */
static void jh_setup(jh_fixture_t *f, const char *queue, const char *hold, bool imessage) {
    memset(f, 0, sizeof(*f));
    f->saved_queue = jh_dup_env("HU_JOB_QUEUE");
    f->saved_hold = jh_dup_env(HU_JOB_HOLD_ENV);
    if (queue)
        setenv("HU_JOB_QUEUE", queue, 1);
    else
        unsetenv("HU_JOB_QUEUE");
    if (hold)
        setenv(HU_JOB_HOLD_ENV, hold, 1);
    else
        unsetenv(HU_JOB_HOLD_ENV);
    hu_daemon_job_queue_reset_for_test();
    hu_daemon_job_hold_reset_for_test();
    f->alloc = hu_system_allocator();
    f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
    f->db = hu_sqlite_memory_get_db(&f->mem);
    (void)hu_daemon_job_queue_start(f->db, NULL);
    f->channel.vtable = imessage ? &jh_vt_imessage : &jh_vt_telegram;
    f->ch.channel = &f->channel;
    f->ch.channel_ctx = &f->channel;
    f->provider.name = "mlx_local";
    f->provider.base_url = "http://127.0.0.1:1/v1";
    f->config.providers = &f->provider;
    f->config.providers_len = 1;
    jh_msg(&f->msgs[0], "+15550000001", "iMessage;-;+15550000001", 101, "you around tonight?");
    jh_msg(&f->msgs[1], "+15550000001", "iMessage;-;+15550000001", 102, "call me");
    jh_msg(&f->msgs[2], "+15550000002", "iMessage;-;+15550000002", 103, "other sender");
}

static void jh_teardown(jh_fixture_t *f) {
    hu_mlx_admin_clear_test_health();
    f->mem.vtable->deinit(f->mem.ctx);
    jh_put_env("HU_JOB_QUEUE", f->saved_queue);
    jh_put_env(HU_JOB_HOLD_ENV, f->saved_hold);
    hu_daemon_job_queue_reset_for_test();
    hu_daemon_job_hold_reset_for_test();
}

static int64_t jh_pending(sqlite3 *db) {
    hu_job_queue_counts_t c;
    if (hu_job_queue_repo_counts(db, &c) != HU_OK)
        return -1;
    return c.pending;
}

static int64_t jh_rows(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM jobs", -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void job_hold_live_enqueues_each_message_of_the_batch(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    hu_mlx_admin_set_test_health(false);
    hu_daemon_job_hold_set_now_for_test(1790000500);
    HU_ASSERT_EQ(hu_daemon_job_hold_mode(), HU_GATE_LIVE);
    HU_ASSERT_EQ(jh_pending(f.db), 0);

    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);

    HU_ASSERT_EQ(jh_pending(f.db), 2);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(f.db,
                                    "SELECT kind, contact, channel, idempotency_key, due_at, "
                                    "created_at FROM jobs ORDER BY id",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), HU_JOB_KIND_INBOUND_HOLD);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "+15550000001");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), HU_JOB_HOLD_CHANNEL);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 3), "hold:iMessage;-;+15550000001:101");
    HU_ASSERT_EQ(sqlite3_column_int64(st, 4), 1790000500); /* due now: release when healthy */
    HU_ASSERT_EQ(sqlite3_column_int64(st, 5), 1790000500); /* age runs from the failed turn */
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 3), "hold:iMessage;-;+15550000001:102");
    sqlite3_finalize(st);

    /* The same messages failing again are never held twice. */
    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_TIMEOUT);
    HU_ASSERT_EQ(jh_rows(f.db), 2);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.held, 2);
    HU_ASSERT_EQ(m.hold_duplicates, 2);
    jh_teardown(&f);
}

static void job_hold_shadow_enqueues_nothing(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "shadow", true);
    hu_mlx_admin_set_test_health(false);
    HU_ASSERT_EQ(hu_daemon_job_hold_mode(), HU_GATE_SHADOW);

    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_PROVIDER_UNAVAILABLE);

    HU_ASSERT_EQ(jh_rows(f.db), 0);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.shadow_would_hold, 2);
    HU_ASSERT_EQ(m.held, 0);
    jh_teardown(&f);
}

static void job_hold_live_probe_up_is_a_counted_one_off(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    hu_mlx_admin_set_test_health(true);

    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);

    HU_ASSERT_EQ(jh_rows(f.db), 0);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.transport_one_offs, 1);
    HU_ASSERT_EQ(m.held, 0);
    jh_teardown(&f);
}

static void job_hold_live_skips_non_transport_and_other_channels(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    hu_mlx_admin_set_test_health(false);
    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_PROVIDER_RESPONSE);
    HU_ASSERT_EQ(jh_rows(f.db), 0);
    jh_teardown(&f);

    jh_setup(&f, "live", "live", false); /* telegram */
    hu_mlx_admin_set_test_health(false);
    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);
    HU_ASSERT_EQ(jh_rows(f.db), 0);
    jh_teardown(&f);
}

static void job_hold_live_refuses_payload_over_cap(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    hu_mlx_admin_set_test_health(false);
    memset(f.msgs[0].content, 'a', sizeof(f.msgs[0].content) - 1);
    f.msgs[0].content[sizeof(f.msgs[0].content) - 1] = '\0';

    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);

    HU_ASSERT_EQ(jh_pending(f.db), 1); /* rowid 102 held, 101 (4095 B) not */
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.hold_skipped, 1);
    HU_ASSERT_EQ(m.held, 1);
    jh_teardown(&f);
}

static void job_hold_live_without_queue_runs_as_shadow(void) {
    jh_fixture_t f;
    jh_setup(&f, NULL, "live", true); /* HU_JOB_QUEUE off: no table */
    hu_mlx_admin_set_test_health(false);
    HU_ASSERT_EQ(hu_daemon_job_hold_mode(), HU_GATE_SHADOW);
    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.shadow_would_hold, 2);
    HU_ASSERT_EQ(m.held, 0);
    jh_teardown(&f);
}

static void job_hold_off_does_nothing(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", NULL, true);
    hu_mlx_admin_set_test_health(false);
    HU_ASSERT_EQ(hu_daemon_job_hold_mode(), HU_GATE_OFF);
    hu_daemon_job_hold_metrics_t before;
    hu_daemon_job_hold_metrics(&before);

    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);

    hu_daemon_job_hold_metrics_t after;
    hu_daemon_job_hold_metrics(&after);
    HU_ASSERT_EQ(memcmp(&before, &after, sizeof(before)), 0);
    HU_ASSERT_EQ(jh_rows(f.db), 0);
    jh_teardown(&f);
}

/* ── Release side (daemon_job_release.c) ─────────────────────────────── */

static size_t g_poll_n; /* messages the mock poll returns */

static hu_error_t jh_poll(void *ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    size_t n = g_poll_n < max_msgs ? g_poll_n : max_msgs;
    for (size_t i = 0; i < n; i++)
        jh_msg(&msgs[i], "+15550000009", "iMessage;-;+15550000009", 300 + (int64_t)i, "new one");
    *out_count = n;
    return HU_OK;
}

static int64_t g_replied_rowid; /* the stub says the owner answered this rowid */
static bool jh_replied(void *ctx, const char *chat_id, const char *handle, int64_t rowid) {
    (void)ctx;
    (void)chat_id;
    (void)handle;
    return rowid == g_replied_rowid;
}

/* Hold msgs[0..1] (rowids 101, 102) at `t0` with the model down. */
static void jh_hold_two(jh_fixture_t *f, int64_t t0) {
    f->ch.poll_fn = jh_poll;
    hu_daemon_job_hold_set_now_for_test(t0);
    hu_mlx_admin_set_test_health(false);
    hu_daemon_jobs_on_turn_error(NULL, &f->config, &f->ch, f->msgs, 0, 1, HU_ERR_IO);
}

static int64_t jh_state_count(sqlite3 *db, const char *state) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM jobs WHERE state=?1", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, state, -1, NULL);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void job_hold_payload_round_trips(void) {
    hu_channel_loop_msg_t m, back;
    jh_msg(&m, "+15550000001", "iMessage;+;chat123", 4242, "line one\nline two");
    m.is_group = true;
    m.has_attachment = true;
    m.was_edited = true;
    snprintf(m.reply_to_guid, sizeof(m.reply_to_guid), "%s", "PARENT-GUID");
    unsigned char buf[HU_JOB_PAYLOAD_MAX];
    size_t len = 0;
    HU_ASSERT_EQ(hu_job_hold_encode(&m, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_EQ(hu_job_hold_decode(buf, len, m.session_key, &back), HU_OK);
    HU_ASSERT_EQ(memcmp(&m, &back, sizeof(m)), 0);
    /* Truncated or foreign payloads are refused, never half-decoded. */
    HU_ASSERT_EQ(hu_job_hold_decode(buf, len - 1, m.session_key, &back), HU_ERR_PARSE);
    buf[0] = 9;
    HU_ASSERT_EQ(hu_job_hold_decode(buf, len, m.session_key, &back), HU_ERR_PARSE);
}

static void job_hold_release_appends_held_messages_once(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    jh_hold_two(&f, 1790000000);
    HU_ASSERT_EQ(jh_pending(f.db), 2);

    /* Model back 70 min later; the poll itself returns one new message. */
    hu_mlx_admin_set_test_health(true);
    hu_daemon_job_hold_set_now_for_test(1790004200);
    g_poll_n = 1;
    hu_channel_loop_msg_t batch[16];
    memset(batch, 0, sizeof(batch));
    size_t count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);

    HU_ASSERT_EQ(count, 3);
    HU_ASSERT_EQ(batch[0].message_id, 101); /* held first, oldest first */
    HU_ASSERT_EQ(batch[1].message_id, 102);
    HU_ASSERT_EQ(batch[2].message_id, 300); /* then what the poll returned */
    HU_ASSERT_EQ(memcmp(&batch[0], &f.msgs[0], sizeof(batch[0])), 0);
    HU_ASSERT_EQ(memcmp(&batch[1], &f.msgs[1], sizeof(batch[1])), 0);
    HU_ASSERT_EQ(jh_state_count(f.db, "done"), 2);
    HU_ASSERT_EQ(jh_pending(f.db), 0);

    /* Next tick: nothing left to hand out. */
    hu_daemon_job_hold_set_now_for_test(1790004260);
    g_poll_n = 0;
    count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.released, 2);

    /* The released turn fails again: the messages are not held a second time. */
    hu_mlx_admin_set_test_health(false);
    hu_daemon_jobs_on_turn_error(NULL, &f.config, &f.ch, f.msgs, 0, 1, HU_ERR_IO);
    HU_ASSERT_EQ(jh_pending(f.db), 0);
    jh_teardown(&f);
}

static void job_hold_release_waits_while_model_down(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    jh_hold_two(&f, 1790000000);
    hu_daemon_job_hold_set_now_for_test(1790000600); /* still down */
    g_poll_n = 0;
    hu_channel_loop_msg_t batch[16];
    size_t count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_EQ(jh_pending(f.db), 2);
    jh_teardown(&f);
}

static void job_hold_release_cancels_what_the_owner_answered(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    jh_hold_two(&f, 1790000000);
    g_replied_rowid = 101;
    hu_daemon_job_hold_set_replied_fn_for_test(jh_replied);
    hu_mlx_admin_set_test_health(true);
    hu_daemon_job_hold_set_now_for_test(1790003000);
    g_poll_n = 0;
    hu_channel_loop_msg_t batch[16];
    size_t count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);

    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(batch[0].message_id, 102);
    HU_ASSERT_EQ(jh_state_count(f.db, "canceled"), 1);
    HU_ASSERT_EQ(jh_state_count(f.db, "done"), 1);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.canceled, 1);
    g_replied_rowid = 0;
    jh_teardown(&f);
}

static void job_hold_expires_after_three_hours_and_notifies_once(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "live", true);
    jh_hold_two(&f, 1790000000);
    hu_owner_notify_test_reset();
    /* Model back, but 3 h + 1 s later: expiry runs before any release. */
    hu_mlx_admin_set_test_health(true);
    hu_daemon_job_hold_set_now_for_test(1790000000 + HU_JOB_HOLD_MAX_AGE_S + 1);
    g_poll_n = 0;
    hu_channel_loop_msg_t batch[16];
    size_t count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);

    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_EQ(jh_state_count(f.db, "expired"), 2);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1);
    HU_ASSERT_NOT_NULL(strstr(hu_owner_notify_test_last_body(), "held 2 messages"));
    /* The banner names no one and quotes nothing. */
    HU_ASSERT_NULL(strstr(hu_owner_notify_test_last_body(), "+1555"));
    HU_ASSERT_NULL(strstr(hu_owner_notify_test_last_body(), "call me"));

    /* Nothing left to expire: no second banner. */
    hu_daemon_job_hold_set_now_for_test(1790000000 + HU_JOB_HOLD_MAX_AGE_S + 120);
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1);
    jh_teardown(&f);
}

static void job_hold_shadow_release_changes_nothing(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", "shadow", true);
    jh_hold_two(&f, 1790000000);
    g_replied_rowid = 102;
    hu_daemon_job_hold_set_replied_fn_for_test(jh_replied);
    hu_mlx_admin_set_test_health(true);
    hu_daemon_job_hold_set_now_for_test(1790004200);
    g_poll_n = 1;
    hu_channel_loop_msg_t batch[16];
    size_t count = 0;
    HU_ASSERT_EQ(hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, batch, 16, &count), HU_OK);

    HU_ASSERT_EQ(count, 1); /* only what the poll returned */
    HU_ASSERT_EQ(batch[0].message_id, 300);
    HU_ASSERT_EQ(jh_rows(f.db), 0);
    hu_daemon_job_hold_metrics_t m;
    hu_daemon_job_hold_metrics(&m);
    HU_ASSERT_EQ(m.shadow_would_release, 1);
    HU_ASSERT_EQ(m.shadow_would_cancel, 1);
    HU_ASSERT_EQ(m.released, 0);
    g_replied_rowid = 0;
    jh_teardown(&f);
}

static void job_hold_off_poll_is_byte_identical(void) {
    jh_fixture_t f;
    jh_setup(&f, "live", NULL, true);
    f.ch.poll_fn = jh_poll;
    /* A due held row the OFF path must leave alone. */
    unsigned char payload[HU_JOB_PAYLOAD_MAX];
    size_t len = 0;
    HU_ASSERT_EQ(hu_job_hold_encode(&f.msgs[0], payload, sizeof(payload), &len), HU_OK);
    hu_job_spec_t spec = {.kind = HU_JOB_KIND_INBOUND_HOLD,
                          .payload = payload,
                          .payload_len = len,
                          .contact = f.msgs[0].session_key,
                          .channel = HU_JOB_HOLD_CHANNEL,
                          .due_at = 1790000000,
                          .idempotency_key = "hold:x:101"};
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &spec, 1790000000, NULL, NULL), HU_OK);
    hu_mlx_admin_set_test_health(true);
    hu_daemon_job_hold_set_now_for_test(1790004200);
    g_poll_n = 2;

    hu_channel_loop_msg_t direct[16], wrapped[16];
    memset(direct, 0, sizeof(direct));
    memset(wrapped, 0, sizeof(wrapped));
    size_t n_direct = 0, n_wrapped = 0;
    hu_error_t e_direct = jh_poll(f.ch.channel_ctx, &f.alloc, direct, 16, &n_direct);
    hu_error_t e_wrapped =
        hu_daemon_jobs_poll(&f.ch, &f.alloc, NULL, &f.config, wrapped, 16, &n_wrapped);

    HU_ASSERT_EQ(e_wrapped, e_direct);
    HU_ASSERT_EQ(n_wrapped, n_direct);
    HU_ASSERT_EQ(memcmp(direct, wrapped, sizeof(direct)), 0);
    HU_ASSERT_EQ(jh_pending(f.db), 1); /* not claimed, not released */
    jh_teardown(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_job_hold_tests(void) {
    HU_TEST_SUITE("daemon job hold");
    HU_RUN_TEST(job_hold_decide_truth_table);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(job_hold_live_enqueues_each_message_of_the_batch);
    HU_RUN_TEST(job_hold_shadow_enqueues_nothing);
    HU_RUN_TEST(job_hold_live_probe_up_is_a_counted_one_off);
    HU_RUN_TEST(job_hold_live_skips_non_transport_and_other_channels);
    HU_RUN_TEST(job_hold_live_refuses_payload_over_cap);
    HU_RUN_TEST(job_hold_live_without_queue_runs_as_shadow);
    HU_RUN_TEST(job_hold_off_does_nothing);
    HU_RUN_TEST(job_hold_payload_round_trips);
    HU_RUN_TEST(job_hold_release_appends_held_messages_once);
    HU_RUN_TEST(job_hold_release_waits_while_model_down);
    HU_RUN_TEST(job_hold_release_cancels_what_the_owner_answered);
    HU_RUN_TEST(job_hold_expires_after_three_hours_and_notifies_once);
    HU_RUN_TEST(job_hold_shadow_release_changes_nothing);
    HU_RUN_TEST(job_hold_off_poll_is_byte_identical);
#endif
}
