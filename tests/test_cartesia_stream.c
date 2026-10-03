#include "human/core/allocator.h"
#include "human/tts/cartesia_stream.h"
#include "test_framework.h"
#include <string.h>

static void cartesia_stream_mock_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_t *s = NULL;
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, "test-key", "voice-1", "sonic-test", &s), HU_OK);
    HU_ASSERT_NOT_NULL(s);

    HU_ASSERT_EQ(hu_cartesia_stream_send_generation(s, &alloc, "ctx-a", "Hello", true), HU_OK);

    void *pcm = NULL;
    size_t plen = 0;
    bool done = false;
    HU_ASSERT_EQ(hu_cartesia_stream_recv_next(s, &alloc, &pcm, &plen, &done), HU_OK);
    HU_ASSERT_NOT_NULL(pcm);
    HU_ASSERT_EQ(plen, 64u * sizeof(float));
    HU_ASSERT_FALSE(done);
    alloc.free(alloc.ctx, pcm, plen);

    /* The context is still open: no "done" until it is flushed. */
    HU_ASSERT_EQ(hu_cartesia_stream_recv_next_wait(s, &alloc, 40, &pcm, &plen, &done),
                 HU_ERR_TIMEOUT);
    HU_ASSERT_NULL(pcm);
    HU_ASSERT_FALSE(done);

    HU_ASSERT_EQ(hu_cartesia_stream_flush_context(s, &alloc, "ctx-a"), HU_OK);
    HU_ASSERT_EQ(hu_cartesia_stream_recv_next(s, &alloc, &pcm, &plen, &done), HU_OK);
    HU_ASSERT_NULL(pcm);
    HU_ASSERT_TRUE(done);

    HU_ASSERT_EQ(hu_cartesia_stream_cancel_context(s, &alloc, "ctx-a"), HU_OK);
    hu_cartesia_stream_close(s, &alloc);
}

static void cartesia_stream_open_null_key_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_t *s = NULL;
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, NULL, "voice-1", "sonic-test", &s),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(s);
}

static void cartesia_stream_open_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, "test-key", "voice-1", "sonic-test", NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void cartesia_stream_close_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_close(NULL, &alloc);
}

static void cartesia_stream_cancel_unknown_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_t *s = NULL;
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, "test-key", "v1", "sonic", &s), HU_OK);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_EQ(hu_cartesia_stream_cancel_context(s, &alloc, "nonexistent"), HU_OK);
    hu_cartesia_stream_close(s, &alloc);
}

static void cartesia_stream_recv_after_done_returns_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_t *s = NULL;
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, "test-key", "v1", "sonic", &s), HU_OK);
    HU_ASSERT_EQ(hu_cartesia_stream_send_generation(s, &alloc, "ctx1", "Hello", true), HU_OK);

    void *pcm = NULL;
    size_t plen = 0;
    bool done = false;
    HU_ASSERT_EQ(hu_cartesia_stream_recv_next(s, &alloc, &pcm, &plen, &done), HU_OK);
    if (pcm)
        alloc.free(alloc.ctx, pcm, plen);
    HU_ASSERT_EQ(hu_cartesia_stream_flush_context(s, &alloc, "ctx1"), HU_OK);

    while (!done) {
        pcm = NULL;
        HU_ASSERT_EQ(hu_cartesia_stream_recv_next(s, &alloc, &pcm, &plen, &done), HU_OK);
        if (pcm)
            alloc.free(alloc.ctx, pcm, plen);
    }
    HU_ASSERT_TRUE(done);

    hu_cartesia_stream_close(s, &alloc);
}

/* A reply streamed in several pieces: each piece's audio arrives while the context is
 * open, and "done" only follows the flush, after the last audio. A caller that waits for
 * "done" before flushing waits for nothing (the gateway did, stalling spoken turns). */
static void cartesia_stream_done_only_after_flush(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_stream_t *s = NULL;
    HU_ASSERT_EQ(hu_cartesia_stream_open(&alloc, "test-key", "v1", "sonic", &s), HU_OK);
    HU_ASSERT_EQ(hu_cartesia_stream_send_generation(s, &alloc, "c", "Not bad,", true), HU_OK);
    HU_ASSERT_EQ(hu_cartesia_stream_send_generation(s, &alloc, "c", " just got home.", true),
                 HU_OK);

    void *pcm = NULL;
    size_t plen = 0;
    bool done = false;
    unsigned chunks = 0;
    hu_error_t err;
    while ((err = hu_cartesia_stream_recv_next_wait(s, &alloc, 40, &pcm, &plen, &done)) == HU_OK &&
           !done) {
        HU_ASSERT_NOT_NULL(pcm);
        alloc.free(alloc.ctx, pcm, plen);
        pcm = NULL;
        chunks++;
    }
    HU_ASSERT_EQ(chunks, 2u);
    HU_ASSERT_EQ(err, HU_ERR_TIMEOUT);
    HU_ASSERT_FALSE(done);

    HU_ASSERT_EQ(hu_cartesia_stream_flush_context(s, &alloc, "c"), HU_OK);
    HU_ASSERT_EQ(hu_cartesia_stream_recv_next_wait(s, &alloc, 40, &pcm, &plen, &done), HU_OK);
    HU_ASSERT_NULL(pcm);
    HU_ASSERT_TRUE(done);
    hu_cartesia_stream_close(s, &alloc);
}

void run_cartesia_stream_tests(void) {
    HU_TEST_SUITE("Cartesia stream");
    HU_RUN_TEST(cartesia_stream_mock_roundtrip);
    HU_RUN_TEST(cartesia_stream_open_null_key_fails);
    HU_RUN_TEST(cartesia_stream_open_null_out_fails);
    HU_RUN_TEST(cartesia_stream_close_null_safe);
    HU_RUN_TEST(cartesia_stream_cancel_unknown_context);
    HU_RUN_TEST(cartesia_stream_recv_after_done_returns_done);
    HU_RUN_TEST(cartesia_stream_done_only_after_flush);
}
