/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM download resume journal (net/rom_journal.h). Step-0 contract test: the
 * 88-byte on-disk header layout, the version/magic constants, and the stub's
 * fail-closed behavior. WF2 lane 2D lands the real kill-9-mid-download resume
 * test (N-1 chunks, SIGKILL, restart, only chunk N re-fetched). */

#include "test/test_helpers.h"
#include "net/rom_journal.h"
#include <string.h>

static int test_rom_journal_header_layout(void)
{
    int failures = 0;
    TEST("rom_journal: on-disk header is exactly 88 bytes") {
        ASSERT(sizeof(struct rom_journal_header) == 88);
        ASSERT(ROM_JOURNAL_MAGIC_LEN == 8);
        ASSERT(strlen(ROM_JOURNAL_MAGIC) == ROM_JOURNAL_MAGIC_LEN);
        ASSERT(ROM_JOURNAL_VERSION == 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_journal_stub_fails_closed(void)
{
    int failures = 0;
    TEST("rom_journal: step-0 stub open fails closed; discard is idempotent") {
        uint8_t root[32] = {0}, whole[32] = {0};
        /* NULL path → NULL. */
        ASSERT(rom_journal_open(NULL, root, whole, 4096, 1) == NULL);
        /* count_done on a NULL handle is a safe 0; is_done is safe false. */
        ASSERT(rom_journal_count_done(NULL) == 0);
        ASSERT(!rom_journal_is_done(NULL, 0));
        /* close(NULL) must not crash. */
        rom_journal_close(NULL);
        /* discard of a nonexistent file is success (nothing to remove). */
        ASSERT(rom_journal_discard("/tmp/zcl-nonexistent-journal-xyz.part.journal"));
        PASS();
    } _test_next:;
    return failures;
}

int test_rom_journal_resume(void)
{
    int failures = 0;
    failures += test_rom_journal_header_layout();
    failures += test_rom_journal_stub_fails_closed();
    return failures;
}
