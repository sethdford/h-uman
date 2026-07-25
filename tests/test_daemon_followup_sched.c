/* tests/test_daemon_followup_sched.c
 *
 * hu_daemon_followup_sched_tick — follow-up watcher scheduling tick
 * (src/daemon/daemon_followup_sched.c, carved from daemon.c per the
 * file-size-ceiling ratchet). Under HU_IS_TEST the chat.db scanner
 * (hu_imessage_find_unreplied_read) is stubbed, so these tests pin the
 * orchestrator's guard clauses and channel filtering — the paths that run
 * on EVERY daemon tick regardless of platform:
 *   - NULL agent / persona / channels are hard no-ops (never crash — the
 *     tick sits inside the service loop's hot path)
 *   - non-imessage channels are filtered out before any contact scan
 *   - zero channels / zero contacts complete without side effects
 */

#include "human/agent.h"
#include "human/daemon.h"
#include "human/persona.h"
#include "test_framework.h"

#include <string.h>

static const char *fs_cli_name(void *ctx) {
    (void)ctx;
    return "cli";
}

static void test_followup_sched_null_inputs_are_noops(void) {
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));

    /* NULL agent, NULL channels, and agent-without-persona must all return
     * without touching the channel array. Reaching the assertion proves the
     * guard clause held (no deref of the zeroed vtable). */
    hu_daemon_followup_sched_tick(NULL, &ch, 1);
    hu_daemon_followup_sched_tick(&agent, NULL, 1);
    hu_daemon_followup_sched_tick(&agent, &ch, 1); /* persona == NULL */
    HU_ASSERT_TRUE(agent.persona == NULL);
}

static void test_followup_sched_skips_non_imessage_channels(void) {
    /* A cli-named channel with warm contacts: the imessage filter must
     * reject it before any contact scan; a NULL-vtable channel must be
     * skipped rather than dereferenced. */
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    hu_contact_profile_t contact;
    memset(&contact, 0, sizeof(contact));
    contact.contact_id = (char *)"+15551230000";
    contact.warmth_level = (char *)"high";
    persona.contacts = &contact;
    persona.contacts_count = 1;

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &persona;

    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = fs_cli_name;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;

    hu_service_channel_t chans[2];
    memset(chans, 0, sizeof(chans));
    chans[0].channel = &channel; /* named "cli" — filtered by name */
    /* chans[1].channel == NULL — must be skipped, not dereferenced */

    hu_daemon_followup_sched_tick(&agent, chans, 2);
    hu_daemon_followup_sched_tick(&agent, chans, 0); /* zero-count no-op */

    /* the tick never mutates persona/contacts on the filtered path */
    HU_ASSERT_EQ(persona.contacts_count, (size_t)1);
    HU_ASSERT_STR_EQ(contact.warmth_level, "high");
}

void run_daemon_followup_sched_tests(void) {
    HU_TEST_SUITE("daemon followup sched tick");
    HU_RUN_TEST(test_followup_sched_null_inputs_are_noops);
    HU_RUN_TEST(test_followup_sched_skips_non_imessage_channels);
}
