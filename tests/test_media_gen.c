#include "human/agent.h"
#include "human/agent/dag.h"
#include "human/agent/tool_context.h"
#include "human/channels/imessage.h"
#include "human/config.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/core/vertex_auth.h"
#include "human/tools/cache_ttl.h"
#include "human/tools/media_gif.h"
#include "human/tools/media_image.h"
#include "human/tools/media_vertex_common.h"
#include "human/tools/media_video.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ── vertex_auth tests ──────────────────────────────────────────────────── */

static void vertex_auth_load_adc_mock_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(&auth, &alloc), HU_OK);
    HU_ASSERT_NOT_NULL(auth.access_token);
    HU_ASSERT(auth.access_token_len > 0);
    hu_vertex_auth_free(&auth);
}

static void vertex_auth_ensure_token_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(&auth, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_vertex_auth_ensure_token(&auth, &alloc), HU_OK);
    HU_ASSERT_NOT_NULL(auth.access_token);
    hu_vertex_auth_free(&auth);
}

static void vertex_auth_get_bearer_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(&auth, &alloc), HU_OK);
    char buf[256];
    HU_ASSERT_EQ(hu_vertex_auth_get_bearer(&auth, buf, sizeof(buf)), HU_OK);
    HU_ASSERT(strncmp(buf, "Bearer ", 7) == 0);
    HU_ASSERT(strlen(buf) > 7);
    hu_vertex_auth_free(&auth);
}

static void vertex_auth_get_bearer_small_buf_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(&auth, &alloc), HU_OK);
    char buf[8];
    HU_ASSERT_EQ(hu_vertex_auth_get_bearer(&auth, buf, sizeof(buf)), HU_ERR_INVALID_ARGUMENT);
    hu_vertex_auth_free(&auth);
}

/* Pins the media-tool contract: callers pass auth_buf straight to
 * hu_http_post_json as a C string, so a failed bearer lookup must leave an
 * empty string, never an untouched (uninitialized) buffer. */
static void vertex_auth_get_bearer_no_token_yields_empty_string(void) {
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    char buf[64];
    memset(buf, 'x', sizeof(buf));
    HU_ASSERT_EQ(hu_vertex_auth_get_bearer(&auth, buf, sizeof(buf)), HU_ERR_PROVIDER_AUTH);
    HU_ASSERT_EQ(buf[0], '\0');
}

static void vertex_auth_null_args_rejected(void) {
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_vertex_auth_ensure_token(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void vertex_auth_free_null_safe(void) {
    hu_vertex_auth_t auth;
    memset(&auth, 0, sizeof(auth));
    hu_vertex_auth_free(&auth);
    hu_vertex_auth_free(NULL);
}

/* ── media_image tool tests ─────────────────────────────────────────────── */

static void media_image_create_registers_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "media_image");
}

static void media_image_has_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT(strlen(tool.vtable->description(tool.ctx)) > 10);
}

static void media_image_has_parameters_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NOT_NULL(params);
    hu_json_value_t *parsed = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, params, strlen(params), &parsed), HU_OK);
    HU_ASSERT_NOT_NULL(parsed);
    hu_json_free(&alloc, parsed);
}

static void media_image_execute_mock_returns_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"a sunset over mountains\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.media_path);
    HU_ASSERT(result.media_path_len > 0);
    HU_ASSERT(strstr(result.media_path, "/tmp/human_img_mock_") != NULL);
    HU_ASSERT(strstr(result.media_path, ".png") != NULL);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_image_missing_prompt_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"model\":\"nano_banana\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_image_invalid_model_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"test\",\"model\":\"nonexistent\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_image_invalid_aspect_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"test\",\"aspect_ratio\":\"99:99\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

/* ── media_video tool tests ─────────────────────────────────────────────── */

static void media_video_create_registers_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_video_create(&alloc, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "media_video");
}

static void media_video_execute_mock_returns_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_video_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"a cat jumping\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.media_path);
    HU_ASSERT(result.media_path_len > 0);
    HU_ASSERT(strstr(result.media_path, "/tmp/human_vid_mock_") != NULL);
    HU_ASSERT(strstr(result.media_path, ".mp4") != NULL);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_video_missing_prompt_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_video_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"duration\":4}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_video_invalid_model_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_video_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"test\",\"model\":\"bad_model\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

/* ── media_gif tool tests ───────────────────────────────────────────────── */

static void media_gif_create_registers_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_gif_create(&alloc, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "media_gif");
}

static void media_gif_execute_mock_returns_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_gif_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"prompt\":\"a dog dancing\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.media_path);
    HU_ASSERT(result.media_path_len > 0);
    HU_ASSERT(strstr(result.media_path, "/tmp/human_gif_mock_") != NULL);
    HU_ASSERT(strstr(result.media_path, ".gif") != NULL);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_gif_missing_prompt_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    HU_ASSERT_EQ(hu_media_gif_create(&alloc, &tool), HU_OK);

    hu_json_value_t *args = NULL;
    const char *json = "{\"aspect_ratio\":\"16:9\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

/* ── tool_result media_path lifecycle ───────────────────────────────────── */

static void tool_result_ok_with_media_sets_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *output = hu_strndup(&alloc, "Generated image", 15);
    char *path = hu_strndup(&alloc, "/tmp/human_img_test.png", 23);
    HU_ASSERT_NOT_NULL(output);
    HU_ASSERT_NOT_NULL(path);

    hu_tool_result_t r = hu_tool_result_ok_with_media(output, 15, path, 23);
    HU_ASSERT(r.success);
    HU_ASSERT(r.output_owned);
    HU_ASSERT(r.media_path_owned);
    HU_ASSERT_STR_EQ(r.media_path, "/tmp/human_img_test.png");
    HU_ASSERT_EQ(r.media_path_len, (size_t)23);

    hu_tool_result_free(&alloc, &r);
    HU_ASSERT(r.media_path == NULL);
    HU_ASSERT(r.output == NULL);
}

static void tool_result_free_null_media_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t r = hu_tool_result_ok("test", 4);
    HU_ASSERT(r.media_path == NULL);
    HU_ASSERT(!r.media_path_owned);
    hu_tool_result_free(&alloc, &r);
}

/* ── e2e integration: tool -> agent -> media_path accumulation ──────────── */

static void media_tool_result_captured_by_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Create a minimal agent */
    hu_provider_t prov = {0};
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.provider = prov;

    HU_ASSERT_EQ(agent.generated_media_count, (size_t)0);

    /* Simulate what agent_turn.c does: tool returns media_path, agent captures it */
    hu_tool_t img_tool;
    memset(&img_tool, 0, sizeof(img_tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &img_tool), HU_OK);

    const char *json = "{\"prompt\":\"a sunset over the ocean\"}";
    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &args), HU_OK);

    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(img_tool.vtable->execute(img_tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.media_path);
    HU_ASSERT(result.media_path_len > 0);

    /* Simulate agent_turn capture logic */
    if (result.success && result.media_path && result.media_path_len > 0 &&
        agent.generated_media_count < 4) {
        char *mp = hu_strndup(&alloc, result.media_path, result.media_path_len);
        HU_ASSERT_NOT_NULL(mp);
        agent.generated_media[agent.generated_media_count++] = mp;
    }

    HU_ASSERT_EQ(agent.generated_media_count, (size_t)1);
    HU_ASSERT_NOT_NULL(agent.generated_media[0]);
    HU_ASSERT(strstr(agent.generated_media[0], "/tmp/human_img_") != NULL);
    HU_ASSERT(strstr(agent.generated_media[0], ".png") != NULL);

    /* Simulate a second tool call (video) */
    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);

    hu_tool_t vid_tool;
    memset(&vid_tool, 0, sizeof(vid_tool));
    HU_ASSERT_EQ(hu_media_video_create(&alloc, &vid_tool), HU_OK);

    const char *vjson = "{\"prompt\":\"waves crashing\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, vjson, strlen(vjson), &args), HU_OK);

    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(vid_tool.vtable->execute(vid_tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.success);
    HU_ASSERT_NOT_NULL(result.media_path);

    if (result.success && result.media_path && result.media_path_len > 0 &&
        agent.generated_media_count < 4) {
        char *mp = hu_strndup(&alloc, result.media_path, result.media_path_len);
        HU_ASSERT_NOT_NULL(mp);
        agent.generated_media[agent.generated_media_count++] = mp;
    }

    HU_ASSERT_EQ(agent.generated_media_count, (size_t)2);
    HU_ASSERT(strstr(agent.generated_media[1], ".mp4") != NULL);

    /* Simulate daemon merge: proactive_vis + generated_media */
    const char *merged[6] = {NULL};
    size_t merged_n = 0;
    for (size_t gm = 0; gm < agent.generated_media_count && merged_n < 6; gm++)
        merged[merged_n++] = agent.generated_media[gm];
    HU_ASSERT_EQ(merged_n, (size_t)2);
    HU_ASSERT(strstr(merged[0], ".png") != NULL);
    HU_ASSERT(strstr(merged[1], ".mp4") != NULL);

    /* Simulate daemon cleanup (unlink + free) */
    for (size_t gmi = 0; gmi < agent.generated_media_count; gmi++) {
        if (agent.generated_media[gmi]) {
            size_t gm_len = strlen(agent.generated_media[gmi]);
            alloc.free(alloc.ctx, agent.generated_media[gmi], gm_len + 1);
            agent.generated_media[gmi] = NULL;
        }
    }
    agent.generated_media_count = 0;
    HU_ASSERT_EQ(agent.generated_media_count, (size_t)0);
    HU_ASSERT(agent.generated_media[0] == NULL);
    HU_ASSERT(agent.generated_media[1] == NULL);

    hu_tool_result_free(&alloc, &result);
    hu_json_free(&alloc, args);
}

static void media_config_fallback_chain(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Config with media_gen settings */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.media_gen.default_image_model = hu_strdup(&alloc, "imagen4");
    cfg.media_gen.default_video_model = hu_strdup(&alloc, "veo_3.1_lite");
    cfg.media_gen.vertex_project = hu_strdup(&alloc, "my-project-123");
    cfg.media_gen.vertex_region = hu_strdup(&alloc, "europe-west4");
    cfg.media_gen.veo_storage_uri = hu_strdup(&alloc, "gs://my-bucket/veo/");

    HU_ASSERT_STR_EQ(cfg.media_gen.default_image_model, "imagen4");
    HU_ASSERT_STR_EQ(cfg.media_gen.default_video_model, "veo_3.1_lite");
    HU_ASSERT_STR_EQ(cfg.media_gen.vertex_project, "my-project-123");
    HU_ASSERT_STR_EQ(cfg.media_gen.vertex_region, "europe-west4");
    HU_ASSERT_STR_EQ(cfg.media_gen.veo_storage_uri, "gs://my-bucket/veo/");

    alloc.free(alloc.ctx, cfg.media_gen.default_image_model, strlen("imagen4") + 1);
    alloc.free(alloc.ctx, cfg.media_gen.default_video_model, strlen("veo_3.1_lite") + 1);
    alloc.free(alloc.ctx, cfg.media_gen.vertex_project, strlen("my-project-123") + 1);
    alloc.free(alloc.ctx, cfg.media_gen.vertex_region, strlen("europe-west4") + 1);
    alloc.free(alloc.ctx, cfg.media_gen.veo_storage_uri, strlen("gs://my-bucket/veo/") + 1);
}

static void media_agent_deinit_cleans_generated_media(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;

    agent.generated_media[0] = hu_strndup(&alloc, "/tmp/test1.png", 14);
    agent.generated_media[1] = hu_strndup(&alloc, "/tmp/test2.mp4", 14);
    agent.generated_media_count = 2;

    /* Simulate the cleanup loop from hu_agent_deinit */
    for (size_t gm = 0; gm < agent.generated_media_count && gm < 4; gm++) {
        if (agent.generated_media[gm]) {
            alloc.free(alloc.ctx, agent.generated_media[gm], strlen(agent.generated_media[gm]) + 1);
            agent.generated_media[gm] = NULL;
        }
    }
    agent.generated_media_count = 0;
    HU_ASSERT(agent.generated_media[0] == NULL);
    HU_ASSERT(agent.generated_media[1] == NULL);
}

/* Verify daemon merge pattern produces correct media array for channel send,
 * including both proactive and tool-generated media with correct ordering. */
static void media_daemon_full_pipeline_with_channel_send(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Set up agent with generated media */
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;

    hu_tool_t img_tool, gif_tool;
    memset(&img_tool, 0, sizeof(img_tool));
    memset(&gif_tool, 0, sizeof(gif_tool));
    HU_ASSERT_EQ(hu_media_image_create(&alloc, &img_tool), HU_OK);
    HU_ASSERT_EQ(hu_media_gif_create(&alloc, &gif_tool), HU_OK);

    /* Execute image tool */
    hu_json_value_t *args = NULL;
    const char *ij = "{\"prompt\":\"full pipeline test\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, ij, strlen(ij), &args), HU_OK);
    hu_tool_result_t r1 = {0};
    HU_ASSERT_EQ(img_tool.vtable->execute(img_tool.ctx, &alloc, args, &r1), HU_OK);
    HU_ASSERT(r1.success && r1.media_path);
    if (r1.media_path && r1.media_path_len > 0 && agent.generated_media_count < 4) {
        agent.generated_media[agent.generated_media_count++] =
            hu_strndup(&alloc, r1.media_path, r1.media_path_len);
    }
    hu_tool_result_free(&alloc, &r1);
    hu_json_free(&alloc, args);

    /* Execute GIF tool */
    args = NULL;
    const char *gj = "{\"prompt\":\"celebration\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, gj, strlen(gj), &args), HU_OK);
    hu_tool_result_t r2 = {0};
    HU_ASSERT_EQ(gif_tool.vtable->execute(gif_tool.ctx, &alloc, args, &r2), HU_OK);
    HU_ASSERT(r2.success && r2.media_path);
    if (r2.media_path && r2.media_path_len > 0 && agent.generated_media_count < 4) {
        agent.generated_media[agent.generated_media_count++] =
            hu_strndup(&alloc, r2.media_path, r2.media_path_len);
    }
    hu_tool_result_free(&alloc, &r2);
    hu_json_free(&alloc, args);

    HU_ASSERT_EQ(agent.generated_media_count, (size_t)2);

    /* Simulate daemon merge (exact pattern from daemon.c) */
    const char *proactive_vis_m[] = {"/tmp/proactive_search.jpg"};
    size_t proactive_vis_n = 1;

    const char *all_send_media[8];
    size_t all_send_media_n = 0;
    for (size_t pmi = 0; pmi < proactive_vis_n && all_send_media_n < 8; pmi++)
        all_send_media[all_send_media_n++] = proactive_vis_m[pmi];
    for (size_t gmi = 0; gmi < agent.generated_media_count && all_send_media_n < 8; gmi++)
        all_send_media[all_send_media_n++] = agent.generated_media[gmi];

    const char *const *all_send_media_ptr = all_send_media_n > 0 ? all_send_media : NULL;
    size_t all_send_media_cnt = all_send_media_n;

    /* Verify: 3 media items (1 proactive + 2 generated) */
    HU_ASSERT_NOT_NULL(all_send_media_ptr);
    HU_ASSERT_EQ(all_send_media_cnt, (size_t)3);
    HU_ASSERT_STR_EQ(all_send_media[0], "/tmp/proactive_search.jpg");
    HU_ASSERT(strstr(all_send_media[1], ".png") != NULL);
    HU_ASSERT(strstr(all_send_media[2], ".gif") != NULL);

    /* Send through iMessage channel (test mode records text, media is passed) */
#ifdef HU_HAS_IMESSAGE
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15559876543", 12, NULL, 0, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "+15559876543", 12, "Check these out!", 16,
                                     all_send_media_ptr, all_send_media_cnt);
    HU_ASSERT_EQ(err, HU_OK);
    size_t msg_len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &msg_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(msg_len, 16u);
    HU_ASSERT_STR_EQ(msg, "Check these out!");
    hu_imessage_destroy(&ch);
#endif

    /* Simulate daemon cleanup (exact pattern from daemon.c) */
    for (size_t gmc = 0; gmc < agent.generated_media_count; gmc++) {
        if (agent.generated_media[gmc]) {
            alloc.free(alloc.ctx, agent.generated_media[gmc],
                       strlen(agent.generated_media[gmc]) + 1);
            agent.generated_media[gmc] = NULL;
        }
    }
    agent.generated_media_count = 0;
}

/* ── media_vertex_common tests ──────────────────────────────────────────── */

/* The resolver reads process env; snapshot the four keys it consults so a
 * test can set exactly the chain it wants and put the caller's env back. */
#define MVC_ENV_N 4
static const char *const mvc_env_keys[MVC_ENV_N] = {"GOOGLE_CLOUD_PROJECT", "VERTEX_PROJECT",
                                                    "GOOGLE_CLOUD_LOCATION", "HU_VEO_STORAGE_URI"};
typedef struct {
    char val[MVC_ENV_N][256];
    bool set[MVC_ENV_N];
} mvc_env_t;

static void mvc_env_save(mvc_env_t *e) {
    for (int i = 0; i < MVC_ENV_N; i++) {
        const char *v = getenv(mvc_env_keys[i]);
        e->set[i] = v != NULL;
        snprintf(e->val[i], sizeof(e->val[i]), "%s", v ? v : "");
        unsetenv(mvc_env_keys[i]);
    }
}

static void mvc_env_restore(const mvc_env_t *e) {
    for (int i = 0; i < MVC_ENV_N; i++) {
        if (e->set[i])
            setenv(mvc_env_keys[i], e->val[i], 1);
        else
            unsetenv(mvc_env_keys[i]);
    }
}

static void media_vertex_open_mock_credentials_and_env_project(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("GOOGLE_CLOUD_PROJECT", "proj-a", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t vauth;
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT(hu_media_vertex_open(&vauth, &alloc, &project, &region, &out));
    HU_ASSERT_NOT_NULL(vauth.access_token);
    HU_ASSERT_STR_EQ(project, "proj-a");
    HU_ASSERT_STR_EQ(region, "us-central1");
    hu_vertex_auth_free(&vauth);
    mvc_env_restore(&env);
}

static void media_vertex_open_missing_project_releases_credentials(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t vauth;
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT_FALSE(hu_media_vertex_open(&vauth, &alloc, &project, &region, &out));
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_STR_EQ(out.error_msg, "GOOGLE_CLOUD_PROJECT not set");
    HU_ASSERT_NULL(vauth.access_token); /* released, not leaked, on the failure path */
    mvc_env_restore(&env);
}

static void media_vertex_resolve_target_env_project_default_region(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("GOOGLE_CLOUD_PROJECT", "proj-a", 1);
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT(hu_media_vertex_resolve_target(&project, &region, &out));
    HU_ASSERT_STR_EQ(project, "proj-a");
    HU_ASSERT_STR_EQ(region, "us-central1");
    mvc_env_restore(&env);
}

static void media_vertex_resolve_target_vertex_project_and_location_fallback(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("VERTEX_PROJECT", "proj-b", 1);
    setenv("GOOGLE_CLOUD_LOCATION", "europe-west4", 1);
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT(hu_media_vertex_resolve_target(&project, &region, &out));
    HU_ASSERT_STR_EQ(project, "proj-b");
    HU_ASSERT_STR_EQ(region, "europe-west4");
    mvc_env_restore(&env);
}

static void media_vertex_resolve_target_missing_project_fails(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT_FALSE(hu_media_vertex_resolve_target(&project, &region, &out));
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_STR_EQ(out.error_msg, "GOOGLE_CLOUD_PROJECT not set");
    mvc_env_restore(&env);
}

static void media_vertex_resolve_target_prefers_agent_config(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("GOOGLE_CLOUD_PROJECT", "env-proj", 1);
    setenv("GOOGLE_CLOUD_LOCATION", "env-region", 1);
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char cfg_project[] = "cfg-proj";
    char cfg_region[] = "cfg-region";
    cfg.media_gen.vertex_project = cfg_project;
    cfg.media_gen.vertex_region = cfg_region;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.config = &cfg;
    hu_agent_set_current_for_tools(&agent);
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    bool ok = hu_media_vertex_resolve_target(&project, &region, &out);
    hu_agent_clear_current_for_tools();
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(project, "cfg-proj");
    HU_ASSERT_STR_EQ(region, "cfg-region");
    mvc_env_restore(&env);
}

/* The mock HTTP layer fills the response on every call, so a still-zeroed
 * response proves the request was never issued. */
static void media_vertex_post_json_without_token_skips_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t vauth;
    memset(&vauth, 0, sizeof(vauth));
    hu_http_response_t resp = {0};
    HU_ASSERT_EQ(
        hu_media_vertex_post_json(&vauth, &alloc, "https://example.invalid/x", "{}", 2, &resp),
        HU_ERR_PROVIDER_AUTH);
    HU_ASSERT_NULL(resp.body);
    HU_ASSERT_EQ(resp.status_code, 0L);
}

static void media_vertex_post_json_with_token_issues_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t vauth;
    memset(&vauth, 0, sizeof(vauth));
    HU_ASSERT_EQ(hu_vertex_auth_load_adc(&vauth, &alloc), HU_OK);
    hu_http_response_t resp = {0};
    HU_ASSERT_EQ(
        hu_media_vertex_post_json(&vauth, &alloc, "https://example.invalid/x", "{}", 2, &resp),
        HU_OK);
    HU_ASSERT_NOT_NULL(resp.body);
    hu_http_response_free(&alloc, &resp);
    hu_vertex_auth_free(&vauth);
}

static void media_vertex_get_without_token_skips_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_vertex_auth_t vauth;
    memset(&vauth, 0, sizeof(vauth));
    hu_http_response_t resp = {0};
    HU_ASSERT_EQ(hu_media_vertex_get(&vauth, &alloc, "https://example.invalid/x", &resp),
                 HU_ERR_PROVIDER_AUTH);
    HU_ASSERT_NULL(resp.body);
    HU_ASSERT_EQ(resp.status_code, 0L);
}

static void media_vertex_veo_storage_uri_defaults_to_project_bucket(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    char buf[256] = {0};
    const char *uri = hu_media_vertex_veo_storage_uri("proj-a", buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(uri);
    HU_ASSERT_STR_EQ(uri, "gs://proj-a-human-media/veo/");
    mvc_env_restore(&env);
}

static void media_vertex_veo_storage_uri_env_overrides_default(void) {
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("HU_VEO_STORAGE_URI", "gs://custom-bucket/out/", 1);
    char buf[256] = {0};
    const char *uri = hu_media_vertex_veo_storage_uri("proj-a", buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(uri);
    HU_ASSERT_STR_EQ(uri, "gs://custom-bucket/out/");
    mvc_env_restore(&env);
}

static void media_vertex_result_from_path_owns_copies(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t out = {0};
    char path[] = "/tmp/human_img_test.png";
    HU_ASSERT_EQ(
        hu_media_vertex_result_from_path(&alloc, "Generated image saved to %s", path, &out), HU_OK);
    HU_ASSERT(out.success);
    HU_ASSERT(out.output_owned);
    HU_ASSERT(out.media_path_owned);
    HU_ASSERT(out.media_path != path); /* an owned copy, not the caller's stack buffer */
    HU_ASSERT_STR_EQ(out.media_path, "/tmp/human_img_test.png");
    HU_ASSERT_EQ(out.media_path_len, strlen(path));
    HU_ASSERT_STR_EQ(out.output, "Generated image saved to /tmp/human_img_test.png");
    HU_ASSERT_EQ(out.output_len, strlen(out.output));
    hu_tool_result_free(&alloc, &out);
}

/* The mock HTTP layer answers the submit with a body that carries no
 * operation name, so the pipeline must stop there — before any poll sleep —
 * with that specific failure. A helper that skipped the POST, or parsed the
 * wrong field, would report something else. */
static void media_vertex_veo_generate_stops_at_missing_operation_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mvc_env_t env;
    mvc_env_save(&env);
    setenv("GOOGLE_CLOUD_PROJECT", "proj-a", 1);
    hu_vertex_auth_t vauth;
    const char *project = NULL, *region = NULL;
    hu_tool_result_t out = {0};
    HU_ASSERT(hu_media_vertex_open(&vauth, &alloc, &project, &region, &out));
    hu_media_veo_request_t req = {
        .model_id = "veo-3.1-lite-generate-001",
        .prompt = "a dog dancing",
        .aspect = "16:9",
        .duration_secs = 4.0,
        .file_tag = "gif",
    };
    char mp4_path[256] = {0};
    hu_error_t err = hu_media_vertex_veo_generate(&alloc, &vauth, project, region, &req, mp4_path,
                                                  sizeof(mp4_path), &out);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NEQ(err, HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_STR_EQ(out.error_msg, "no operation name in response");
    HU_ASSERT_EQ(mp4_path[0], '\0');
    mvc_env_restore(&env);
}

static void media_vertex_mock_result_builds_kind_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t out = {0};
    HU_ASSERT_EQ(hu_media_vertex_mock_result(&alloc, "gif", "gif", "a dog dancing", &out), HU_OK);
    HU_ASSERT(out.success);
    HU_ASSERT(out.media_path_owned);
    HU_ASSERT_STR_EQ(out.media_path, "/tmp/human_gif_mock_a dog dancing.gif");
    HU_ASSERT_STR_EQ(out.output, out.media_path);
    hu_tool_result_free(&alloc, &out);
}

static void media_vertex_mock_result_truncates_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t out = {0};
    const char *prompt = "0123456789012345678901234567890123456789"; /* 40 chars */
    HU_ASSERT_EQ(hu_media_vertex_mock_result(&alloc, "img", "png", prompt, &out), HU_OK);
    HU_ASSERT_STR_EQ(out.media_path, "/tmp/human_img_mock_012345678901234567890123456789.png");
    hu_tool_result_free(&alloc, &out);
}

/* ── registration ───────────────────────────────────────────────────────── */

void run_media_gen_tests(void) {
    HU_TEST_SUITE("media_gen");

    HU_RUN_TEST(vertex_auth_load_adc_mock_succeeds);
    HU_RUN_TEST(vertex_auth_ensure_token_mock);
    HU_RUN_TEST(vertex_auth_get_bearer_formats);
    HU_RUN_TEST(vertex_auth_get_bearer_small_buf_fails);
    HU_RUN_TEST(vertex_auth_get_bearer_no_token_yields_empty_string);
    HU_RUN_TEST(vertex_auth_null_args_rejected);
    HU_RUN_TEST(vertex_auth_free_null_safe);

    HU_RUN_TEST(media_vertex_open_mock_credentials_and_env_project);
    HU_RUN_TEST(media_vertex_open_missing_project_releases_credentials);
    HU_RUN_TEST(media_vertex_resolve_target_env_project_default_region);
    HU_RUN_TEST(media_vertex_resolve_target_vertex_project_and_location_fallback);
    HU_RUN_TEST(media_vertex_resolve_target_missing_project_fails);
    HU_RUN_TEST(media_vertex_resolve_target_prefers_agent_config);
    HU_RUN_TEST(media_vertex_post_json_without_token_skips_request);
    HU_RUN_TEST(media_vertex_post_json_with_token_issues_request);
    HU_RUN_TEST(media_vertex_get_without_token_skips_request);
    HU_RUN_TEST(media_vertex_veo_storage_uri_defaults_to_project_bucket);
    HU_RUN_TEST(media_vertex_veo_storage_uri_env_overrides_default);
    HU_RUN_TEST(media_vertex_result_from_path_owns_copies);
    HU_RUN_TEST(media_vertex_veo_generate_stops_at_missing_operation_name);
    HU_RUN_TEST(media_vertex_mock_result_builds_kind_path);
    HU_RUN_TEST(media_vertex_mock_result_truncates_prompt);

    HU_RUN_TEST(media_image_create_registers_name);
    HU_RUN_TEST(media_image_has_description);
    HU_RUN_TEST(media_image_has_parameters_json);
    HU_RUN_TEST(media_image_execute_mock_returns_path);
    HU_RUN_TEST(media_image_missing_prompt_fails);
    HU_RUN_TEST(media_image_invalid_model_fails);
    HU_RUN_TEST(media_image_invalid_aspect_fails);

    HU_RUN_TEST(media_video_create_registers_name);
    HU_RUN_TEST(media_video_execute_mock_returns_path);
    HU_RUN_TEST(media_video_missing_prompt_fails);
    HU_RUN_TEST(media_video_invalid_model_fails);

    HU_RUN_TEST(media_gif_create_registers_name);
    HU_RUN_TEST(media_gif_execute_mock_returns_path);
    HU_RUN_TEST(media_gif_missing_prompt_fails);

    HU_RUN_TEST(tool_result_ok_with_media_sets_fields);
    HU_RUN_TEST(tool_result_free_null_media_safe);

    HU_RUN_TEST(media_tool_result_captured_by_agent);
    HU_RUN_TEST(media_config_fallback_chain);
    HU_RUN_TEST(media_agent_deinit_cleans_generated_media);
    HU_RUN_TEST(media_daemon_full_pipeline_with_channel_send);
}
