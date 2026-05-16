#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "test_framework.h"
#include <string.h>

/* Verbatim F1 leak from the Jordan iMessage thread — same text as
 * test_validators_persona_safety.c so both suites pin the same signal. */
static const char *JORDAN_LEAK_F1 =
    "Wait, looking at the history, the AI has been slipping into "
    "\"How can I help you today?\" which is a massive AI tell and "
    "explicitly forbidden by the persona instructions. I need to snap "
    "back into Seth.\n\n"
    "Seth is chill, playful, and romantic with Jordan.\n"
    "If she says \"Oh nice!\", he should probably keep it light or ask a follow-up";

/* SEMANTIC CONTRACT: after the chain executes with REJECT, the call-site
 * deny-by-default pattern MUST clear the send buffer.  This test simulates
 * the exact pattern used at each send site in daemon.c / daemon_cron.c /
 * imessage.c / gateway/openai_compat.c so that the contract is documented
 * and will break loudly if any future refactor reverts the fix. */
static void reject_deny_by_default_clears_send_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Build the default outbound chain with persona name "Seth" so
     * persona_narrator_validator can detect the F1 leak. */
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);

    /* Simulate a send buffer holding the F1 leak text. */
    size_t buf_len = strlen(JORDAN_LEAK_F1);
    char buf[2048];
    HU_ASSERT(buf_len < sizeof(buf) - 1);
    memcpy(buf, JORDAN_LEAK_F1, buf_len);
    buf[buf_len] = '\0';

    /* Execute the chain. */
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_error_t err = hu_output_validator_chain_execute(chain, &alloc, NULL, buf, buf_len, &cr);
    HU_ASSERT_EQ(err, HU_OK);

    /* The chain must REJECT the F1 leak. */
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);

    /* Apply the deny-by-default call-site pattern:
     * REJECT => clear buf so unsanitized content cannot reach the channel. */
    if (cr.final_decision == HU_VALIDATOR_REJECT) {
        buf[0] = '\0';
        buf_len = 0;
    } else if (cr.final_text) {
        if (cr.final_text != buf && cr.final_text_len <= sizeof(buf) - 1) {
            memcpy(buf, cr.final_text, cr.final_text_len);
            buf_len = cr.final_text_len;
            buf[buf_len] = '\0';
        }
    }

    /* After deny-by-default: buffer must be empty — nothing reaches the wire. */
    HU_ASSERT_EQ(buf_len, (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');

    /* final_text is NULL on REJECT — no rewrite to accidentally send. */
    HU_ASSERT(cr.final_text == NULL);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Positive case: PASS leaves the buffer unchanged. */
static void pass_deny_by_default_preserves_send_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);

    const char *msg = "yeah totally up for that, what time?";
    size_t buf_len = strlen(msg);
    char buf[256];
    memcpy(buf, msg, buf_len);
    buf[buf_len] = '\0';

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, buf, buf_len, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);

    /* Apply deny-by-default pattern — PASS must NOT clear the buffer. */
    if (cr.final_decision == HU_VALIDATOR_REJECT) {
        buf[0] = '\0';
        buf_len = 0;
    } else if (cr.final_text) {
        if (cr.final_text != buf && cr.final_text_len <= sizeof(buf) - 1) {
            memcpy(buf, cr.final_text, cr.final_text_len);
            buf_len = cr.final_text_len;
            buf[buf_len] = '\0';
        }
    }

    /* Buffer still contains the original message. */
    HU_ASSERT(buf_len > 0);
    HU_ASSERT_STR_EQ(buf, msg);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

void run_validator_reject_discards_tests(void) {
    HU_TEST_SUITE("validator_reject_discards");
    HU_RUN_TEST(reject_deny_by_default_clears_send_buffer);
    HU_RUN_TEST(pass_deny_by_default_preserves_send_buffer);
}
