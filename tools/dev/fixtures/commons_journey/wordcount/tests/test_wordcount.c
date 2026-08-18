/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Acceptance tests for wordcount. They cover the reused counters and the
 * created one together, so a candidate that omits wordcount_longest_line()
 * cannot link, let alone pass. */

#include "wordcount/wordcount.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *what, size_t got, size_t want)
{
    printf("wordcount: FAIL %s got=%zu want=%zu\n", what, got, want);
    return 1;
}

int main(void)
{
    static const char sample[] = "one two\nthree four five\nsix\n";
    size_t len = strlen(sample);
    struct wordcount_report r;
    wordcount_measure(sample, len, &r);
    if (r.lines != 3) return fail("lines", r.lines, 3);
    if (r.words != 6) return fail("words", r.words, 6);
    if (r.bytes != len) return fail("bytes", r.bytes, len);
    /* "three four five" is 15 bytes and is the longest line. */
    if (r.longest_line != 15) return fail("longest_line", r.longest_line, 15);

    if (wordcount_longest_line("no newline at all", 17) != 17)
        return fail("unterminated longest",
                    wordcount_longest_line("no newline at all", 17), 17);
    if (wordcount_longest_line("\n\n\n", 3) != 0)
        return fail("empty longest", wordcount_longest_line("\n\n\n", 3), 0);
    if (wordcount_longest_line(NULL, 9) != 0)
        return fail("null longest", wordcount_longest_line(NULL, 9), 0);
    printf("wordcount: OK\n");
    return 0;
}
