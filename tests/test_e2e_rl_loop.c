/* tests/test_e2e_rl_loop.c — Phase 6 deterministic closed-loop wiring proof. */
#include "test_framework.h"
#include "hu_e2e_closed_loop.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/crypto.h"
#include "human/ml/dpo.h"
#include "human/ml/dpo_real.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/rl_trainer.h"
#include "human/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef struct e2e_mock_provider_ctx {
    const hu_model_t *policy;
    int load_count;
} e2e_mock_provider_ctx_t;

static float e2e_policy_signature(const hu_model_t *policy) {
    if (!policy || !policy->vtable || !policy->vtable->get_params)
        return 0.f;
    hu_ml_tensor_t *params = NULL;
    size_t count = 0;
    if (policy->vtable->get_params(policy->ctx, &params, &count) != HU_OK || count == 0)
        return 0.f;
    double sum = 0.0;
    for (size_t pi = 0; pi < count; pi++) {
        if (params[pi].dtype != HU_ML_DTYPE_F32 || !params[pi].data)
            continue;
        const float *f = (const float *)params[pi].data;
        size_t n = params[pi].size_bytes / sizeof(float);
        for (size_t i = 0; i < n; i++)
            sum += (double)f[i];
    }
    return (float)sum;
}

static hu_error_t e2e_mock_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                              const char *system_prompt, size_t system_prompt_len,
                                              const char *message, size_t message_len,
                                              const char *model, size_t model_len,
                                              double temperature, char **out, size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    if (!ctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    e2e_mock_provider_ctx_t *m = (e2e_mock_provider_ctx_t *)ctx;
    float sig = e2e_policy_signature(m->policy);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "e2e_sig_%u", (unsigned)(sig * 1000000.f));
    if (n <= 0)
        return HU_ERR_PROVIDER_RESPONSE;
    size_t len = (size_t)n;
    char *copy = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, buf, len);
    copy[len] = '\0';
    *out = copy;
    *out_len = len;
    return HU_OK;
}

static hu_error_t e2e_mock_load_adapter(void *ctx, hu_allocator_t *alloc, const char *adapter_path,
                                        size_t adapter_path_len, const char *adapter_id,
                                        size_t adapter_id_len) {
    (void)alloc;
    (void)adapter_path;
    (void)adapter_path_len;
    (void)adapter_id;
    (void)adapter_id_len;
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    ((e2e_mock_provider_ctx_t *)ctx)->load_count++;
    return HU_OK;
}

static hu_error_t e2e_mock_unload_adapter(void *ctx, const char *adapter_id,
                                          size_t adapter_id_len) {
    (void)adapter_id;
    (void)adapter_id_len;
    (void)ctx;
    return HU_OK;
}

static const char *e2e_mock_name(void *ctx) {
    (void)ctx;
    return "e2e_huml_mock";
}

static void e2e_mock_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(e2e_mock_provider_ctx_t));
}

static const hu_provider_vtable_t E2E_MOCK_VTABLE = {
    .chat_with_system = e2e_mock_chat_with_system,
    .load_adapter = e2e_mock_load_adapter,
    .unload_adapter = e2e_mock_unload_adapter,
    .get_name = e2e_mock_name,
    .deinit = e2e_mock_deinit,
};

static hu_reaction_kind_t parse_reaction_kind(const char *s) {
    if (!s)
        return HU_REACTION_UNKNOWN;
    if (strcmp(s, "love") == 0)
        return HU_REACTION_LOVE;
    if (strcmp(s, "like") == 0)
        return HU_REACTION_LIKE;
    if (strcmp(s, "dislike") == 0)
        return HU_REACTION_DISLIKE;
    if (strcmp(s, "laugh") == 0)
        return HU_REACTION_LAUGH;
    if (strcmp(s, "emphasize") == 0)
        return HU_REACTION_EMPHASIZE;
    if (strcmp(s, "question") == 0)
        return HU_REACTION_KIND_QUESTION;
    return HU_REACTION_UNKNOWN;
}

static hu_error_t read_file(hu_allocator_t *alloc, const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return HU_OK;
}

static void load_persona_seed(const char ***out_held_out, size_t *out_n_prompts) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(read_file(&alloc, "tests/fixtures/e2e_persona_seed.json", &json, &json_len),
                 HU_OK);
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, json_len, &root), HU_OK);
    alloc.free(alloc.ctx, json, json_len + 1);

    hu_json_value_t *held = hu_json_object_get(root, "held_out_prompts");
    HU_ASSERT_NOT_NULL(held);
    HU_ASSERT_EQ(held->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(held->data.array.len, 100u);
    const char **held_out =
        (const char **)alloc.alloc(alloc.ctx, 100 * sizeof(const char *));
    for (size_t i = 0; i < 100; i++) {
        hu_json_value_t *p = held->data.array.items[i];
        char *dup = (char *)alloc.alloc(alloc.ctx, p->data.string.len + 1);
        memcpy(dup, p->data.string.ptr, p->data.string.len);
        dup[p->data.string.len] = '\0';
        held_out[i] = dup;
    }
    *out_held_out = held_out;
    *out_n_prompts = 100;
    hu_json_free(&alloc, root);
}

typedef struct e2e_loaded_events {
    hu_reaction_event_t *events;
    char **event_storage;
    size_t n;
} e2e_loaded_events_t;

static char *json_strdup_field(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *d = (char *)alloc->alloc(alloc->ctx, n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void load_reaction_signals(e2e_loaded_events_t *loaded, hu_e2e_reaction_aux_t **out_aux) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(read_file(&alloc, "tests/fixtures/e2e_reaction_signals.json", &json, &json_len),
                 HU_OK);
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, json_len, &root), HU_OK);
    alloc.free(alloc.ctx, json, json_len + 1);

    hu_json_value_t *events = hu_json_object_get(root, "events");
    HU_ASSERT_NOT_NULL(events);
    size_t n = events->data.array.len;
    HU_ASSERT_EQ(n, 50u);

    loaded->n = n;
    loaded->events =
        (hu_reaction_event_t *)alloc.alloc(alloc.ctx, n * sizeof(hu_reaction_event_t));
    loaded->event_storage = (char **)alloc.alloc(alloc.ctx, n * 4 * sizeof(char *));
    hu_e2e_reaction_aux_t *aux =
        (hu_e2e_reaction_aux_t *)alloc.alloc(alloc.ctx, n * sizeof(hu_e2e_reaction_aux_t));
    memset(loaded->events, 0, n * sizeof(hu_reaction_event_t));
    memset(aux, 0, n * sizeof(hu_e2e_reaction_aux_t));

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *obj = events->data.array.items[i];
        loaded->event_storage[i * 4 + 0] =
            json_strdup_field(&alloc, hu_json_get_string(obj, "channel_id"));
        loaded->event_storage[i * 4 + 1] =
            json_strdup_field(&alloc, hu_json_get_string(obj, "target_thread_id"));
        loaded->event_storage[i * 4 + 2] =
            json_strdup_field(&alloc, hu_json_get_string(obj, "target_message_ref"));
        loaded->event_storage[i * 4 + 3] =
            json_strdup_field(&alloc, hu_json_get_string(obj, "sender_handle"));
        loaded->events[i].channel_id = loaded->event_storage[i * 4 + 0];
        loaded->events[i].target_thread_id = loaded->event_storage[i * 4 + 1];
        loaded->events[i].target_message_ref = loaded->event_storage[i * 4 + 2];
        loaded->events[i].sender_handle = loaded->event_storage[i * 4 + 3];
        loaded->events[i].kind = parse_reaction_kind(hu_json_get_string(obj, "kind"));
        loaded->events[i].polarity =
            (hu_reaction_polarity_t)(int)hu_json_get_number(obj, "polarity", 0);
        loaded->events[i].timestamp_unix = (int64_t)hu_json_get_number(obj, "timestamp_unix", 0);
        loaded->events[i].is_removal = (int)hu_json_get_number(obj, "is_removal", 0);
        hu_json_value_t *ax = hu_json_object_get(obj, "_aux");
        HU_ASSERT_NOT_NULL(ax);
        (void)ax;
        /* HUML DPO trainer tokenizes space-separated int ids — use fixed ids so
         * trainer.step mutates weights deterministically. */
        aux[i].prompt = json_strdup_field(&alloc, "1 2 3");
        aux[i].response_chosen = json_strdup_field(&alloc, "4 5");
        aux[i].response_rejected = json_strdup_field(&alloc, "6 7");
    }

    *out_aux = aux;
    hu_json_free(&alloc, root);
}

static void free_loaded_events(hu_allocator_t *alloc, e2e_loaded_events_t *loaded,
                               hu_e2e_reaction_aux_t *aux) {
    if (aux)
        hu_e2e_reaction_aux_free(alloc, aux, loaded->n);
    if (loaded->event_storage) {
        for (size_t i = 0; i < loaded->n * 4; i++) {
            if (loaded->event_storage[i])
                alloc->free(alloc->ctx, loaded->event_storage[i],
                            strlen(loaded->event_storage[i]) + 1);
        }
        alloc->free(alloc->ctx, loaded->event_storage, loaded->n * 4 * sizeof(char *));
    }
    if (loaded->events)
        alloc->free(alloc->ctx, loaded->events, loaded->n * sizeof(hu_reaction_event_t));
    memset(loaded, 0, sizeof(*loaded));
}

static void set_up_env(void) {
    srand(42);
    setenv("HU_E2E_FIXED_TIMESTAMP", "1715472000", 1);
    hu_reaction_handler_reset_for_test();
    (void)mkdir(HU_E2E_TMP_ROOT, 0755);
    (void)mkdir(HU_E2E_TMP_ROOT "/proofs", 0755);
}

static void tear_down_env(void) {
    unsetenv("HU_E2E_FIXED_TIMESTAMP");
    hu_reaction_handler_reset_for_test();
}

typedef struct e2e_loop_bundle {
    hu_provider_t provider;
    e2e_mock_provider_ctx_t *mock_ctx;
    hu_rl_trainer_t trainer;
    hu_dpo_collector_t collector;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#endif
} e2e_loop_bundle_t;

static void make_e2e_bundle(hu_allocator_t *alloc, e2e_loop_bundle_t *b) {
    memset(b, 0, sizeof(*b));
#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_EQ(sqlite3_open(":memory:", &b->db), SQLITE_OK);
    HU_ASSERT_EQ(hu_dpo_collector_create(alloc, b->db, 128, &b->collector), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&b->collector), HU_OK);
#else
    HU_SKIP_IF(1, "SQLite required");
#endif

    hu_rl_trainer_config_t tcfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 0.1, /* toy GPT needs a strong step to move responses */
    };
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(alloc, &tcfg, &b->trainer), HU_OK);

    b->mock_ctx = (e2e_mock_provider_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*b->mock_ctx));
    HU_ASSERT_NOT_NULL(b->mock_ctx);
    memset(b->mock_ctx, 0, sizeof(*b->mock_ctx));
    b->mock_ctx->policy = hu_dpo_real_huml_policy_for_test(&b->trainer);
    HU_ASSERT_NOT_NULL(b->mock_ctx->policy);
    b->provider.ctx = b->mock_ctx;
    b->provider.vtable = &E2E_MOCK_VTABLE;
}

static void free_e2e_bundle(hu_allocator_t *alloc, e2e_loop_bundle_t *b) {
    if (b->trainer.vtable && b->trainer.vtable->deinit)
        b->trainer.vtable->deinit(b->trainer.ctx, alloc);
    if (b->provider.vtable && b->provider.vtable->deinit)
        b->provider.vtable->deinit(b->provider.ctx, alloc);
    hu_dpo_collector_deinit(&b->collector);
    if (b->db)
        sqlite3_close(b->db);
}

static void run_closed_loop_case(const char *adapter_suffix, hu_e2e_closed_loop_output_t *out) {
    hu_allocator_t alloc = hu_system_allocator();
    e2e_loop_bundle_t bundle;
    make_e2e_bundle(&alloc, &bundle);

    e2e_loaded_events_t loaded = {0};
    hu_e2e_reaction_aux_t *aux = NULL;
    load_reaction_signals(&loaded, &aux);

    char adapter_path[1024];
    hu_e2e_tmp_path(adapter_path, sizeof(adapter_path), adapter_suffix);

    hu_e2e_closed_loop_input_t in = {
        .provider = &bundle.provider,
        .trainer = &bundle.trainer,
        .collector = &bundle.collector,
        .reaction_events = loaded.events,
        .reaction_aux = aux,
        .reaction_event_count = loaded.n,
        .system_prompt = "respond like the persona seed.",
        .system_prompt_len = strlen("respond like the persona seed."),
        .user_message = "what should i do first?",
        .user_message_len = strlen("what should i do first?"),
        .model = "huml-toy-gpt",
        .model_len = strlen("huml-toy-gpt"),
        .temperature = 0.0,
        .adapter_out_path = adapter_path,
        .adapter_id = "e2e-dpo-001",
    };
    HU_ASSERT_EQ(hu_e2e_closed_loop_run(&in, &alloc, out), HU_OK);

    free_loaded_events(&alloc, &loaded, aux);
    free_e2e_bundle(&alloc, &bundle);
}

static void test_e2e_closed_loop_dpo_shows_measurable_response_change(void) {
    set_up_env();
    hu_e2e_closed_loop_output_t out = {0};
    run_closed_loop_case("proofs/dpo-step-0001/lora.bin", &out);
    HU_ASSERT_TRUE(out.responses_differ);
    HU_ASSERT_GT(out.before_response_len, 0);
    HU_ASSERT_GT(out.after_response_len, 0);
    HU_ASSERT_GE((size_t)out.pairs_consumed, 50u);
    struct stat st;
    HU_ASSERT_EQ(stat(out.adapter_path, &st), 0);
    HU_ASSERT_GT(st.st_size, 0);
    HU_ASSERT_LE(out.elapsed_ms, 30000);
    {
        hu_allocator_t alloc = hu_system_allocator();
        hu_e2e_closed_loop_output_free(&alloc, &out);
    }
    tear_down_env();
}

static void test_e2e_closed_loop_all_synthetic_reactions_become_dpo_pairs(void) {
    set_up_env();
    hu_allocator_t alloc = hu_system_allocator();
    e2e_loop_bundle_t bundle;
    make_e2e_bundle(&alloc, &bundle);

    e2e_loaded_events_t loaded = {0};
    hu_e2e_reaction_aux_t *aux = NULL;
    load_reaction_signals(&loaded, &aux);

    for (size_t i = 0; i < loaded.n; i++) {
        const hu_reaction_event_t *e = &loaded.events[i];
        hu_reaction_handler_register_assistant_message_for_test(
            e->channel_id, e->target_thread_id, e->target_message_ref, aux[i].prompt,
            e->polarity == HU_REACTION_POSITIVE ? aux[i].response_chosen
                                                : aux[i].response_rejected);
    }
    hu_reaction_handler_set_collector(&bundle.collector);
    for (size_t i = 0; i < loaded.n; i++)
        (void)hu_reaction_handler_handle_event(&loaded.events[i]);
    hu_reaction_handler_set_collector(NULL);

    size_t n_pairs = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&bundle.collector, &n_pairs), HU_OK);
    HU_ASSERT_EQ(n_pairs, loaded.n);

    hu_dpo_export_t export_data = {0};
    HU_ASSERT_EQ(hu_dpo_export(&bundle.collector, &alloc, &export_data), HU_OK);
    HU_ASSERT_EQ(export_data.count, 50u);
    for (size_t i = 0; i < export_data.count; i++) {
        HU_ASSERT_TRUE(strstr(export_data.pairs[i].source, "imessage_tapback") != NULL);
    }
    hu_dpo_export_free(&alloc, &export_data);

    free_loaded_events(&alloc, &loaded, aux);
    free_e2e_bundle(&alloc, &bundle);
    tear_down_env();
}

static void test_e2e_closed_loop_provider_after_response_differs_from_before(void) {
    set_up_env();
    hu_e2e_closed_loop_output_t out = {0};
    run_closed_loop_case("proofs/f1-step/lora.bin", &out);
    if (out.before_response_len == out.after_response_len &&
        memcmp(out.before_response, out.after_response, out.before_response_len) == 0) {
        HU_FAIL("F1: provider returned identical response after adapter swap");
    }
    {
        hu_allocator_t alloc = hu_system_allocator();
        hu_e2e_closed_loop_output_free(&alloc, &out);
    }
    tear_down_env();
}

static void compute_file_sha256(hu_allocator_t *alloc, const char *path, uint8_t out[32]) {
    FILE *fp = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    HU_ASSERT_GT(fsize, 0);
    uint8_t *buf = alloc->alloc(alloc->ctx, (size_t)fsize);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(fread(buf, 1, (size_t)fsize, fp), (size_t)fsize);
    fclose(fp);
    hu_sha256(buf, (size_t)fsize, out);
    alloc->free(alloc->ctx, buf, (size_t)fsize);
}

static void run_one_closed_loop_and_hash(hu_allocator_t *alloc, int run_idx, uint8_t out_hash[32]) {
    srand(42);
    char suffix[128];
    snprintf(suffix, sizeof(suffix), "proofs/det-run-%d/lora.bin", run_idx);
    hu_e2e_closed_loop_output_t out = {0};
    run_closed_loop_case(suffix, &out);
    compute_file_sha256(alloc, out.adapter_path, out_hash);
    hu_e2e_closed_loop_output_free(alloc, &out);
}

static void test_e2e_closed_loop_deterministic_run1_vs_run2(void) {
    set_up_env();
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t h1[32], h2[32];
    run_one_closed_loop_and_hash(&alloc, 1, h1);
    run_one_closed_loop_and_hash(&alloc, 2, h2);
    HU_ASSERT_EQ(memcmp(h1, h2, 32), 0);
    tear_down_env();
}

void run_e2e_closed_loop_tests(void) {
    HU_TEST_SUITE("E2E-closed-loop");
    HU_RUN_TEST(test_e2e_closed_loop_dpo_shows_measurable_response_change);
    HU_RUN_TEST(test_e2e_closed_loop_all_synthetic_reactions_become_dpo_pairs);
    HU_RUN_TEST(test_e2e_closed_loop_provider_after_response_differs_from_before);
    HU_RUN_TEST(test_e2e_closed_loop_deterministic_run1_vs_run2);
}
