/* tests/test_compatible_vision.c — an OpenAI-compatible provider learns that its
 * server is text-only.
 *
 * 2026-10-02: every photo a contact sent went to the text-only local model
 * (HTTP 422 "unsupported_modality"), because compatible_supports_vision always
 * answered true. After one rejected image the provider now stops claiming vision,
 * so the daemon's supports_vision check skips image description. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/compatible.h"
#include "test_framework.h"
#include <string.h>

static void make_provider(hu_allocator_t *alloc, hu_provider_t *p) {
    const char url[] = "http://127.0.0.1:8741/v1";
    HU_ASSERT_EQ(hu_compatible_create(alloc, NULL, 0, url, sizeof(url) - 1, p), HU_OK);
}

static bool claims_vision(hu_provider_t *p) {
    return p->vtable->supports_vision && p->vtable->supports_vision(p->ctx);
}

static void request_with(hu_chat_message_t *msg, hu_content_part_t *part, hu_content_part_tag_t tag,
                         hu_chat_request_t *req) {
    memset(part, 0, sizeof(*part));
    part->tag = tag;
    memset(msg, 0, sizeof(*msg));
    msg->role = HU_ROLE_USER;
    msg->content_parts = part;
    msg->content_parts_count = 1;
    memset(req, 0, sizeof(*req));
    req->messages = msg;
    req->messages_count = 1;
}

static void compatible_vision_off_after_image_rejected_as_unsupported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p;
    make_provider(&alloc, &p);
    HU_ASSERT_TRUE(claims_vision(&p)); /* optimistic until the server says otherwise */

    hu_chat_message_t msg;
    hu_content_part_t part;
    hu_chat_request_t req;
    request_with(&msg, &part, HU_CONTENT_PART_IMAGE_BASE64, &req);
    hu_compatible_record_modality_error(&p, HU_ERR_NOT_SUPPORTED, &req);
    HU_ASSERT_FALSE(claims_vision(&p));
    p.vtable->deinit(p.ctx, &alloc);
}

static void compatible_vision_kept_for_other_failures(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p;
    make_provider(&alloc, &p);
    hu_chat_message_t msg;
    hu_content_part_t part;
    hu_chat_request_t req;
    request_with(&msg, &part, HU_CONTENT_PART_IMAGE_URL, &req);
    /* A transient or unrelated error on an image request is not evidence. */
    hu_compatible_record_modality_error(&p, HU_ERR_PROVIDER_RESPONSE, &req);
    hu_compatible_record_modality_error(&p, HU_ERR_IO, &req);
    HU_ASSERT_TRUE(claims_vision(&p));
    /* NOT_SUPPORTED on a text-only request says nothing about images. */
    request_with(&msg, &part, HU_CONTENT_PART_TEXT, &req);
    hu_compatible_record_modality_error(&p, HU_ERR_NOT_SUPPORTED, &req);
    HU_ASSERT_TRUE(claims_vision(&p));
    p.vtable->deinit(p.ctx, &alloc);
}

static void compatible_vision_flag_is_per_provider_and_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t a, b;
    make_provider(&alloc, &a);
    make_provider(&alloc, &b);
    hu_chat_message_t msg;
    hu_content_part_t part;
    hu_chat_request_t req;
    request_with(&msg, &part, HU_CONTENT_PART_VIDEO_URL, &req);
    hu_compatible_record_modality_error(&a, HU_ERR_NOT_SUPPORTED, &req);
    HU_ASSERT_FALSE(claims_vision(&a));
    HU_ASSERT_TRUE(claims_vision(&b)); /* another endpoint keeps its own answer */
    hu_compatible_record_modality_error(NULL, HU_ERR_NOT_SUPPORTED, &req);
    hu_compatible_record_modality_error(&b, HU_ERR_NOT_SUPPORTED, NULL);
    HU_ASSERT_TRUE(claims_vision(&b));
    a.vtable->deinit(a.ctx, &alloc);
    b.vtable->deinit(b.ctx, &alloc);
}

void run_compatible_vision_tests(void) {
    HU_TEST_SUITE("compatible_vision");
    HU_RUN_TEST(compatible_vision_off_after_image_rejected_as_unsupported);
    HU_RUN_TEST(compatible_vision_kept_for_other_failures);
    HU_RUN_TEST(compatible_vision_flag_is_per_provider_and_null_safe);
}
