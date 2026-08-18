/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wordcount(1) — read one text file named on the command line and print its
 * lines, words, bytes and longest line. This is the thing a person actually
 * runs at the end of the journey, so its output is plain and its failures
 * name the file. */

#include "wordcount/wordcount.h"

#include <stdio.h>
#include <stdlib.h>

#define WORDCOUNT_MAX_BYTES (1u << 20)

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: wordcount <file>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "wordcount: cannot read %s\n", argv[1]);
        return 2;
    }
    /* A stranger's ordinary C23 program: it must compile against nothing but
     * libc. The whole point of the journey is that Z23 builds code it did not
     * write and does not link itself into. */
    char *text = malloc(WORDCOUNT_MAX_BYTES); // raw-alloc-ok:plain-libc-fixture
    if (!text) {
        fclose(f);
        fprintf(stderr, "wordcount: out of memory\n");
        return 2;
    }
    size_t len = fread(text, 1, WORDCOUNT_MAX_BYTES, f);
    int truncated = !feof(f);
    fclose(f);
    if (truncated) {
        free(text);
        fprintf(stderr, "wordcount: %s is larger than %u bytes\n", argv[1],
                WORDCOUNT_MAX_BYTES);
        return 2;
    }
    struct wordcount_report r;
    wordcount_measure(text, len, &r);
    free(text);
    printf("lines %zu words %zu bytes %zu longest_line %zu\n", r.lines,
           r.words, r.bytes, r.longest_line);
    return 0;
}
