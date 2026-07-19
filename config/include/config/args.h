/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Node-mode flag/argv parsing surface (config/src/args.c). Split out of
 * src/main.c (pure code motion); called from src/main.c's main() and, for
 * print_usage(), from the CLI client in src/main_cli_modes.c.
 */
#ifndef ZCLASSIC23_CONFIG_ARGS_H
#define ZCLASSIC23_CONFIG_ARGS_H

#include <stdbool.h>

struct app_context;

/* The -help / --help usage text. */
void print_usage(const char *prog);

/* Apply -loglevel=<all|info|warn|error|fatal|off> if present (default: no
 * change). An unrecognized value warns, never aborts. */
void apply_argv_loglevel(void);

/* The node-mode strcmp(argv) flag ladder. Fills *ctx and *show_metrics.
 * Returns -1 to continue booting; a value >= 0 is an exit code the caller
 * must return from main() (e.g. --help / --version -> 0, a bad -profile= or
 * -operator-lane= or the removed -assumevalid -> 1). The silent-ignore of
 * unrecognized flags (with a loud one-line WARNING) is preserved exactly. */
int args_parse_node_options(int argc, char **argv, struct app_context *ctx,
                            bool *show_metrics);

#endif /* ZCLASSIC23_CONFIG_ARGS_H */
