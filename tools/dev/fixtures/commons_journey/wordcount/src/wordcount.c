/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wordcount — see include/wordcount/wordcount.h.
 *
 * Note what is NOT here. Lines, words and bytes are not reimplemented; they
 * come from z23/textstat, which the reuse plan selects before any code is
 * written. wordcount_longest_line() is absent because nothing in the commons
 * provides it, so it is the only thing candidate work is allowed to add. */

#include "wordcount/wordcount.h"

#include "textstat/textstat.h"

void wordcount_measure(const char *text, size_t len,
                       struct wordcount_report *out)
{
    if (!out) return;
    out->lines = textstat_lines(text, len);
    out->words = textstat_words(text, len);
    out->bytes = textstat_bytes(text, len);
    out->longest_line = wordcount_longest_line(text, len);
}
