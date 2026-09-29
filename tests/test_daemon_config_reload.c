/* Contract tests for the daemon's SIGHUP config-reload tick.
 *
 * These assert a real pre/post transition: the config file on disk changes, the
 * flag is raised, one tick runs, and the agent's IN-MEMORY value is different
 * afterwards. That is the contract that was missing — before this module,
 * hu_config_get_and_clear_reload_requested had zero callers, so SIGHUP to the
 * daemon set a flag nobody read.
 *
 * Deliberately NOT asserted: "reload_requested == false" or "summary != NULL".
 * Both hold whether or not a reload did anything (see
 * .claude/rules/tests-that-pin-bugs.md — a test name is a claim, and these
 * names claim the value changed).
 *
 * Hermetic via HOME: hu_config_load resolves $HOME/.human/config.json (and the
 * workspace config beneath the same root), so a temp HOME fully isolates these
 * from the developer's real config. */
#include "test_framework.h"

#include "human/agent.h"
#include "human/agent/instruction_discover.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/daemon/config_reload.h"
#include "human/hook.h"
#include "human/permission.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char reload_prev_home[1024];
static bool reload_prev_home_set = false;
static char reload_test_home[512];

/* Point HOME at a private temp root so hu_config_load reads only what these
 * tests write. */
static void reload_home_setup(void) {
    const char *prev = getenv("HOME");
    reload_prev_home_set = (prev != NULL);
    if (prev)
        snprintf(reload_prev_home, sizeof(reload_prev_home), "%s", prev);

    snprintf(reload_test_home, sizeof(reload_test_home), "/tmp/hu-config-reload-test-%d",
             (int)getpid());
    (void)mkdir(reload_test_home, 0700);

    char human_dir[600];
    snprintf(human_dir, sizeof(human_dir), "%s/.human", reload_test_home);
    (void)mkdir(human_dir, 0700);

    setenv("HOME", reload_test_home, 1);
}

static void reload_home_teardown(void) {
    if (reload_prev_home_set)
        setenv("HOME", reload_prev_home, 1);
    else
        unsetenv("HOME");
}

/* Write $HOME/.human/config.json carrying a single agent.permission_level. */
static void write_permission_config(int level) {
    char path[700];
    snprintf(path, sizeof(path), "%s/.human/config.json", reload_test_home);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    if (!f)
        return;
    fprintf(f, "{\"agent\":{\"permission_level\":%d}}\n", level);
    fclose(f);
}

static void reload_agent_init(hu_agent_t *agent, hu_allocator_t *alloc) {
    memset(agent, 0, sizeof(*agent));
    agent->alloc = alloc;
    agent->permission_base_level = HU_PERM_READ_ONLY;
    agent->permission_level = HU_PERM_READ_ONLY;
    agent->workspace_dir = ".";
    agent->workspace_dir_len = 1;
}

static void reload_agent_cleanup(hu_agent_t *agent) {
    if (agent->hook_registry) {
        hu_hook_registry_destroy(agent->hook_registry, agent->alloc);
        agent->hook_registry = NULL;
    }
    if (agent->instruction_discovery) {
        hu_instruction_discovery_destroy(agent->alloc, agent->instruction_discovery);
        agent->instruction_discovery = NULL;
    }
}

/* THE contract: a config edit on disk plus a raised flag changes the agent's
 * live permission tier within one tick. Two successive transitions
 * (READ_ONLY -> DANGER_FULL_ACCESS -> WORKSPACE_WRITE) prove the tick re-reads
 * the file each time rather than applying one latched value. */
static void reload_tick_applies_changed_permission_level(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reload_home_setup();

    hu_agent_t agent;
    reload_agent_init(&agent, &alloc);

    /* Precondition: the agent is at the starting tier, and the tier the config
     * will ask for is genuinely different. */
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_READ_ONLY);

    write_permission_config((int)HU_PERM_DANGER_FULL_ACCESS);
    hu_config_set_reload_requested();

    HU_ASSERT_TRUE(hu_daemon_config_reload_tick(&agent, NULL));

    /* Postcondition: the in-memory value CHANGED, and to the value the file
     * asked for — not merely "a reload ran". */
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ((int)agent.permission_level, (int)HU_PERM_DANGER_FULL_ACCESS);

    /* Second edit: proves the tick re-reads the file, not a cached value. */
    write_permission_config((int)HU_PERM_WORKSPACE_WRITE);
    hu_config_set_reload_requested();

    HU_ASSERT_TRUE(hu_daemon_config_reload_tick(&agent, NULL));
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_WORKSPACE_WRITE);

    reload_agent_cleanup(&agent);
    reload_home_teardown();
}

/* The tick must be a no-op on the overwhelmingly common path (no SIGHUP), or
 * the daemon would re-read config.json roughly once a second forever. */
static void reload_tick_without_flag_leaves_agent_untouched(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reload_home_setup();

    hu_agent_t agent;
    reload_agent_init(&agent, &alloc);

    /* A config that WOULD change the tier if a reload ran. */
    write_permission_config((int)HU_PERM_DANGER_FULL_ACCESS);
    (void)hu_config_get_and_clear_reload_requested(); /* ensure no stale request */

    HU_ASSERT_FALSE(hu_daemon_config_reload_tick(&agent, NULL));

    /* Unchanged precisely because no reload was requested. */
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_READ_ONLY);

    reload_agent_cleanup(&agent);
    reload_home_teardown();
}

/* One SIGHUP must produce exactly one reload. If the flag were not consumed,
 * every subsequent tick would reload again. */
static void reload_tick_consumes_the_flag_once(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reload_home_setup();

    hu_agent_t agent;
    reload_agent_init(&agent, &alloc);

    write_permission_config((int)HU_PERM_DANGER_FULL_ACCESS);
    hu_config_set_reload_requested();

    HU_ASSERT_TRUE(hu_daemon_config_reload_tick(&agent, NULL));
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_DANGER_FULL_ACCESS);

    /* Second tick, no new SIGHUP: the flag is gone, so no reload happens even
     * though the config on disk still differs from the agent's start value. */
    HU_ASSERT_FALSE(hu_daemon_config_reload_tick(&agent, NULL));

    reload_agent_cleanup(&agent);
    reload_home_teardown();
}

/* Cron-only daemons run with agent == NULL. The tick must still clear the flag
 * (so it does not fire on a later tick that does have an agent) and report that
 * nothing was reloaded. */
static void reload_tick_without_agent_reports_no_reload(void) {
    reload_home_setup();

    write_permission_config((int)HU_PERM_DANGER_FULL_ACCESS);
    hu_config_set_reload_requested();

    HU_ASSERT_FALSE(hu_daemon_config_reload_tick(NULL, NULL));
    /* Flag was consumed, not left pending. */
    HU_ASSERT_FALSE(hu_config_get_and_clear_reload_requested());

    reload_home_teardown();
}

#if !defined(_WIN32)
/* The guard is what keeps a reload from freeing the hook registry under a
 * concurrent gateway turn. Registering one must not change the outcome; a NULL
 * registration must restore the unguarded path for later tests. */
static void reload_tick_runs_under_a_registered_guard(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reload_home_setup();

    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    hu_daemon_config_reload_set_guard(&guard);

    hu_agent_t agent;
    reload_agent_init(&agent, &alloc);

    write_permission_config((int)HU_PERM_DANGER_FULL_ACCESS);
    hu_config_set_reload_requested();

    HU_ASSERT_TRUE(hu_daemon_config_reload_tick(&agent, NULL));
    HU_ASSERT_EQ((int)agent.permission_base_level, (int)HU_PERM_DANGER_FULL_ACCESS);

    /* The mutex must be released, not left held — a second guarded tick would
     * deadlock otherwise. */
    hu_config_set_reload_requested();
    HU_ASSERT_TRUE(hu_daemon_config_reload_tick(&agent, NULL));

    hu_daemon_config_reload_set_guard(NULL);
    reload_agent_cleanup(&agent);
    reload_home_teardown();
}
#endif

void run_daemon_config_reload_tests(void) {
    HU_TEST_SUITE("DaemonConfigReload");
    HU_RUN_TEST(reload_tick_applies_changed_permission_level);
    HU_RUN_TEST(reload_tick_without_flag_leaves_agent_untouched);
    HU_RUN_TEST(reload_tick_consumes_the_flag_once);
    HU_RUN_TEST(reload_tick_without_agent_reports_no_reload);
#if !defined(_WIN32)
    HU_RUN_TEST(reload_tick_runs_under_a_registered_guard);
#endif
}
