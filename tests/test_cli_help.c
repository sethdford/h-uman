/* CLI help routing and init's overwrite decision.
 *
 * `human <cmd> --help` used to reach the command handler, so for commands
 * that ignore their arguments it RAN the command: `service-loop --help`
 * started the daemon loop, `gateway --help` bound the gateway, `init --help`
 * wrote a config, `migrate --help` migrated memory. The dispatcher now asks
 * hu_cli_help_requested() first. scripts/check-cli-help-safety.sh proves the
 * same end to end against the real binary. */
/* fmemopen is POSIX 2008: glibc hides it under -std=c11 without this. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "human/cli_commands.h"
#include "human/cli_help.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

#define ARGV(...)       ((char *[]){__VA_ARGS__, NULL})
#define ARGC(...)       ((int)(sizeof((char *[]){__VA_ARGS__}) / sizeof(char *)))
#define WANTS_HELP(...) hu_cli_help_requested(ARGC(__VA_ARGS__), ARGV(__VA_ARGS__))

static void cli_help_flag_after_command_is_help(void) {
    HU_ASSERT_TRUE(WANTS_HELP("human", "service-loop", "--help"));
    HU_ASSERT_TRUE(WANTS_HELP("human", "gateway", "-h"));
}

static void cli_help_flag_after_subcommand_is_help(void) {
    HU_ASSERT_TRUE(WANTS_HELP("human", "memory", "search", "--help"));
    HU_ASSERT_TRUE(WANTS_HELP("human", "drafts", "--contact", "x", "-h"));
}

static void cli_help_no_flag_is_not_help(void) {
    HU_ASSERT_FALSE(WANTS_HELP("human", "status"));
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "hello"));
    HU_ASSERT_FALSE(hu_cli_help_requested(1, ARGV("human")));
}

static void cli_help_flag_after_end_of_options_is_data(void) {
    /* `--` ends option parsing: a literal "--help" after it is an argument
     * (e.g. text to search for), not a request for help. */
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "--", "--help"));
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "--", "-h"));
}

static void cli_help_lookalikes_are_not_help(void) {
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "--helpful"));
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "-hx"));
    HU_ASSERT_FALSE(WANTS_HELP("human", "memory", "search", "help-me"));
}

static void cli_help_command_word_itself_is_not_scanned(void) {
    /* argv[1] is the command; global `human --help` is handled separately. */
    HU_ASSERT_FALSE(WANTS_HELP("human", "--help"));
}

static void cli_help_null_argv_is_not_help(void) {
    HU_ASSERT_FALSE(hu_cli_help_requested(3, NULL));
    char *holes[] = {"human", "memory", NULL, "--help", NULL};
    HU_ASSERT_TRUE(hu_cli_help_requested(4, holes)); /* NULL entries skipped */
}

static void cli_help_summary_names_the_command_and_description(void) {
    char buf[512];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    HU_ASSERT_NOT_NULL(f);
    hu_cli_print_summary(f, "service-loop", "Run service loop in foreground");
    fclose(f);
    HU_ASSERT_NOT_NULL(strstr(buf, "Usage: human service-loop"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Run service loop in foreground"));
}

/* init with an existing config used to block on `Overwrite? [y/N]` even when
 * stdin was not a terminal, hanging scripts and CI. */
static void init_decision_no_config_proceeds(void) {
    HU_ASSERT_EQ(hu_init_overwrite_decision(false, false, false), HU_INIT_PROCEED);
    HU_ASSERT_EQ(hu_init_overwrite_decision(false, false, true), HU_INIT_PROCEED);
}

static void init_decision_force_overwrites_without_asking(void) {
    HU_ASSERT_EQ(hu_init_overwrite_decision(true, true, false), HU_INIT_PROCEED);
    HU_ASSERT_EQ(hu_init_overwrite_decision(true, true, true), HU_INIT_PROCEED);
}

static void init_decision_existing_config_prompts_only_on_a_tty(void) {
    HU_ASSERT_EQ(hu_init_overwrite_decision(true, false, true), HU_INIT_PROMPT);
    HU_ASSERT_EQ(hu_init_overwrite_decision(true, false, false), HU_INIT_REFUSE);
}

void run_cli_help_tests(void) {
    HU_TEST_SUITE("CLI help routing");
    HU_RUN_TEST(cli_help_flag_after_command_is_help);
    HU_RUN_TEST(cli_help_flag_after_subcommand_is_help);
    HU_RUN_TEST(cli_help_no_flag_is_not_help);
    HU_RUN_TEST(cli_help_flag_after_end_of_options_is_data);
    HU_RUN_TEST(cli_help_lookalikes_are_not_help);
    HU_RUN_TEST(cli_help_command_word_itself_is_not_scanned);
    HU_RUN_TEST(cli_help_null_argv_is_not_help);
    HU_RUN_TEST(cli_help_summary_names_the_command_and_description);
    HU_RUN_TEST(init_decision_no_config_proceeds);
    HU_RUN_TEST(init_decision_force_overwrites_without_asking);
    HU_RUN_TEST(init_decision_existing_config_prompts_only_on_a_tty);
}
