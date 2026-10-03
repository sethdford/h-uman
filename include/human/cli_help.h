#ifndef HU_CLI_HELP_H
#define HU_CLI_HELP_H

#include <stdbool.h>
#include <stdio.h>

/*
 * CLI help routing. `human <command> --help` must describe the command and
 * NEVER run it. Before this existed the flag reached each handler, so commands
 * that ignore their arguments executed: `service-loop --help` started the
 * daemon loop, `gateway --help` bound the gateway, `init --help` wrote a
 * config, `migrate --help` migrated memory, `update --help` hit the network.
 */

/* How a command answers --help. SUMMARY is 0 on purpose: a command-table
 * entry that does not say otherwise can never be executed by --help. */
typedef enum hu_cli_help_style {
    HU_CLI_HELP_SUMMARY = 0, /* print name + description; never call the handler */
    HU_CLI_HELP_SELF,        /* the handler parses --help itself (subcommand-level help) */
    HU_CLI_HELP_BARE,        /* the bare command only prints its full usage */
} hu_cli_help_style_t;

/* True when argv asks for help: "--help" or "-h" at argv[2] or later, before an
 * end-of-options "--". argv[1] is the command itself (global `human --help` is
 * handled by the dispatcher). NULL entries are skipped. */
bool hu_cli_help_requested(int argc, char *const *argv);

/* Print the SUMMARY help for a command. */
void hu_cli_print_summary(FILE *out, const char *name, const char *description);

#endif /* HU_CLI_HELP_H */
