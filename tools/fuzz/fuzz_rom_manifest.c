/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_rom_manifest — libFuzzer harness for the WF2 artifact-protocol wire
 * deserializers (net/rom_fetch.h, net/rom_journal.h): the "RMF" per-chunk
 * manifest blob a seeder returns, and the on-disk resume-journal header +
 * bitmap a restarted download reads back. Both are attacker-reachable:
 * the manifest blob arrives over the wire from any peer claiming to serve
 * ROM artifacts; the journal is read from disk at every download resume,
 * so a corrupted/truncated `.part.journal` (crash mid-write, disk fault,
 * or a hostile local file) must never be partially trusted or overrun a
 * buffer — it must fail closed exactly like rom_journal_open documents.
 *
 * Mirrors tools/fuzz/fuzz_snapshot.c's shape: pure entry points, no
 * sockets, deterministic per iteration. Runs with
 * -fsanitize=fuzzer,address,undefined under clang. */

#include "net/rom_fetch.h"
#include "net/rom_journal.h"
#include "net/rom_seed.h"
#include "util/safe_alloc.h"

#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t g_shutdown_requested = 0;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > (1u << 20))
        return 0; /* libFuzzer convention: 0 means "keep going" */

    /* (1) "RMF" manifest blob parse — the pure, fully-untrusted-input core
     * behind rom_fetch_get_manifest's wire reply. Bounds: version, length
     * multiple-of-32, num_chunks range/cap, and the chunk_root content-fold
     * check must all reject before any array write. */
    {
        uint8_t chunk_root[32];
        memset(chunk_root, 0, sizeof(chunk_root));
        if (size >= 32)
            memcpy(chunk_root, data, 32);
        uint8_t (*out)[32] = zcl_malloc((size_t)ROM_SEED_MAX_CHUNKS * 32,
                                        "fuzz_rom_manifest_out");
        if (out) {
            uint32_t nc = 0;
            (void)rom_fetch_parse_manifest_blob(data, size, chunk_root, out,
                                                ROM_SEED_MAX_CHUNKS, &nc);
            free(out);
        }
    }

    /* (2) Per-chunk content verify: bounded SHA3 compare over an arbitrary
     * data/length split. */
    if (size >= 32) {
        uint8_t expect[32];
        memcpy(expect, data, 32);
        (void)rom_fetch_verify_chunk(data + 32, size - 32, expect);
    }

    /* (3) Resume-journal open: attacker-controlled on-disk bytes under a
     * FIXED, caller-committed manifest identity — mirrors exactly what a
     * resumed download reads back at restart. A header that matches the
     * identity below (so rom_journal_open takes the "resume" branch) plus
     * fuzzed trailing bytes reaches the bitmap-validation logic (short
     * read, stray high bits beyond num_chunks) directly; the harness also
     * varies num_chunks per iteration from the input so different bitmap
     * sizes get exercised across the corpus. */
    {
        static const uint8_t chunk_root[32] = { 0x11 };
        static const uint8_t whole_sha3[32] = { 0x22 };
        const uint32_t chunk_size = 4u * 1024u * 1024u;
        const uint32_t num_chunks = 1u + (uint32_t)(size ? (data[0] % 64) : 7);

        char path[256];
        snprintf(path, sizeof(path), "/tmp/zcl_fuzz_rom_journal_%d.tmp",
                 (int)getpid());
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            struct rom_journal_header h;
            memset(&h, 0, sizeof(h));
            memcpy(h.magic, ROM_JOURNAL_MAGIC, ROM_JOURNAL_MAGIC_LEN);
            h.version = ROM_JOURNAL_VERSION;
            h.chunk_size = chunk_size;
            h.num_chunks = num_chunks;
            memcpy(h.chunk_root, chunk_root, 32);
            memcpy(h.whole_sha3, whole_sha3, 32);
            (void)write(fd, &h, sizeof(h)); /* header matches the identity below */
            (void)write(fd, data, size);    /* fuzzed trailing (bitmap) bytes    */
            close(fd);
        }

        struct rom_journal *j = rom_journal_open(path, chunk_root, whole_sha3,
                                                 chunk_size, num_chunks);
        if (j) {
            (void)rom_journal_is_done(j, 0);
            (void)rom_journal_is_done(j, num_chunks - 1);
            (void)rom_journal_is_done(j, num_chunks); /* out of range */
            (void)rom_journal_count_done(j);
            rom_journal_close(j);
        }
        (void)rom_journal_discard(path);
    }

    return 0;
}
