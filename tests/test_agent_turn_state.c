/* tests/test_agent_turn_state.c — per-turn state tracking for behavior-log stash.
 *
 * Pins the contract for the 4 deferred behavior-log fields (#26):
 *   - tool_count: incremented per tool dispatch
 *   - tool_sequence_hash: FNV-1a 32-bit of "name1|name2|..."
 *   - emotional_register: enum, last-set wins
 *   - persona_delta_kind: enum, last-set wins
 *
 * These tests exercise the public helpers (declared in human/agent.h)
 * against a zero-initialized hu_agent_t — they touch only
 * agent->current_turn_state and have no other dependencies.
 *
 * Spec reference: per-turn state tracking refactor (deferred #26 from
 * 2026-05-20 behavior-log-stash work, completed 2026-05-24).
 */
#include "human/agent.h"
#include "human/agent/self_model.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"
#include <string.h>

/* AC-1: reset clears all four fields. */
static void turn_state_reset_clears_all_fields(void) {
    hu_agent_t a;
    memset(&a, 0, sizeof(a));
    a.current_turn_state.tool_count = 7;
    a.current_turn_state.tool_sequence_hash = 0xdeadbeefu;
    a.current_turn_state.emotional_register = (uint8_t)HU_AGENT_EMOTION_NEGATIVE;
    a.current_turn_state.persona_delta_kind = (uint8_t)HU_PERSONA_DELTA_TONE;

    hu_agent_turn_state_reset(&a);

    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 0);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_sequence_hash, 0);
    HU_ASSERT_EQ((int)a.current_turn_state.emotional_register, (int)HU_AGENT_EMOTION_NEUTRAL);
    HU_ASSERT_EQ((int)a.current_turn_state.persona_delta_kind, 0);
}

/* AC-2: track_tool increments count and seeds + chains the hash. */
static void turn_state_track_tool_increments_count_and_hashes_sequence(void) {
    hu_agent_t a;
    memset(&a, 0, sizeof(a));

    hu_agent_turn_state_track_tool(&a, "shell", 5);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 1);
    uint32_t after_one = a.current_turn_state.tool_sequence_hash;
    HU_ASSERT(after_one != 0); /* seeded from FNV-1a basis, then mixed */

    hu_agent_turn_state_track_tool(&a, "search", 6);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 2);
    HU_ASSERT(a.current_turn_state.tool_sequence_hash != after_one);
}

/* AC-3: order matters — same set of tools in different order produces
 * a different hash. This is the property that makes the hash useful
 * for detecting tool-sequence drift in the behavior log. */
static void turn_state_tool_sequence_hash_is_order_sensitive(void) {
    hu_agent_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    hu_agent_turn_state_track_tool(&a, "shell", 5);
    hu_agent_turn_state_track_tool(&a, "search", 6);

    hu_agent_turn_state_track_tool(&b, "search", 6);
    hu_agent_turn_state_track_tool(&b, "shell", 5);

    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 2);
    HU_ASSERT_EQ((int)b.current_turn_state.tool_count, 2);
    HU_ASSERT(a.current_turn_state.tool_sequence_hash != b.current_turn_state.tool_sequence_hash);
}

/* AC-4: same tools in same order produce the same hash — determinism. */
static void turn_state_tool_sequence_hash_is_deterministic(void) {
    hu_agent_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    hu_agent_turn_state_track_tool(&a, "shell", 5);
    hu_agent_turn_state_track_tool(&a, "search", 6);
    hu_agent_turn_state_track_tool(&a, "read", 4);

    hu_agent_turn_state_track_tool(&b, "shell", 5);
    hu_agent_turn_state_track_tool(&b, "search", 6);
    hu_agent_turn_state_track_tool(&b, "read", 4);

    HU_ASSERT_EQ((int)a.current_turn_state.tool_sequence_hash,
                 (int)b.current_turn_state.tool_sequence_hash);
}

/* AC-5: track_tool ignores NULL / zero-length names without crashing. */
static void turn_state_track_tool_rejects_bad_inputs(void) {
    hu_agent_t a;
    memset(&a, 0, sizeof(a));

    hu_agent_turn_state_track_tool(NULL, "shell", 5);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 0);

    hu_agent_turn_state_track_tool(&a, NULL, 5);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 0);

    hu_agent_turn_state_track_tool(&a, "shell", 0);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_count, 0);
    HU_ASSERT_EQ((int)a.current_turn_state.tool_sequence_hash, 0);
}

/* AC-6: emotional_register setter writes the byte, accepts NULL agent. */
static void turn_state_set_emotional_register_writes_field(void) {
    hu_agent_t a;
    memset(&a, 0, sizeof(a));

    hu_agent_turn_state_set_emotional_register(&a, (uint8_t)HU_AGENT_EMOTION_POSITIVE);
    HU_ASSERT_EQ((int)a.current_turn_state.emotional_register, (int)HU_AGENT_EMOTION_POSITIVE);

    hu_agent_turn_state_set_emotional_register(&a, (uint8_t)HU_AGENT_EMOTION_CAUTIOUS);
    HU_ASSERT_EQ((int)a.current_turn_state.emotional_register, (int)HU_AGENT_EMOTION_CAUTIOUS);

    hu_agent_turn_state_set_emotional_register(NULL,
                                               (uint8_t)HU_AGENT_EMOTION_NEGATIVE); /* no crash */
}

/* AC-7: persona_delta setter writes the byte, accepts NULL agent. */
static void turn_state_set_persona_delta_writes_field(void) {
    hu_agent_t a;
    memset(&a, 0, sizeof(a));

    hu_agent_turn_state_set_persona_delta(&a, (uint8_t)HU_PERSONA_DELTA_TONE);
    HU_ASSERT_EQ((int)a.current_turn_state.persona_delta_kind, (int)HU_PERSONA_DELTA_TONE);

    hu_agent_turn_state_set_persona_delta(NULL, (uint8_t)HU_PERSONA_DELTA_TONE); /* no crash */
}

void run_agent_turn_state_tests(void) {
    HU_TEST_SUITE("agent_turn_state");
    HU_RUN_TEST(turn_state_reset_clears_all_fields);
    HU_RUN_TEST(turn_state_track_tool_increments_count_and_hashes_sequence);
    HU_RUN_TEST(turn_state_tool_sequence_hash_is_order_sensitive);
    HU_RUN_TEST(turn_state_tool_sequence_hash_is_deterministic);
    HU_RUN_TEST(turn_state_track_tool_rejects_bad_inputs);
    HU_RUN_TEST(turn_state_set_emotional_register_writes_field);
    HU_RUN_TEST(turn_state_set_persona_delta_writes_field);
}
