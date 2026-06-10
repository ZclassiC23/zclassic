/* Tor API stubs — used when real Tor is not linked.
 * Provides all symbols needed by tor_integration.c */

#include <stddef.h>
#include <stdint.h>

/* Tor main API stubs */
void *tor_main_configuration_new(void) { return NULL; }
int tor_main_configuration_set_command_line(void *cfg, int argc, char **argv)
{ (void)cfg; (void)argc; (void)argv; return 0; }
int tor_run_main(void *cfg) { (void)cfg; return -1; }
void tor_main_configuration_free(void *cfg) { (void)cfg; }

/* Dynhost webserver stub */
typedef size_t (*dynhost_external_handler_fn)(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t, void *);
void dynhost_webserver_set_external_handler(
    dynhost_external_handler_fn handler, void *ctx)
{ (void)handler; (void)ctx; }

/* Shutdown stub */
void tor_shutdown_event_loop_and_exit(int exitcode) { (void)exitcode; }
