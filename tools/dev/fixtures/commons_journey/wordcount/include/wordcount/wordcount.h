/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wordcount — the small C23 application half of the commons-journey
 * acceptance fixture. It reports lines, words and bytes for a text buffer
 * by REUSING z23/textstat, and reports the longest line, which no package
 * in the commons provides.
 *
 * That split is the whole point of the fixture: everything declared here
 * except wordcount_longest_line() is satisfied by reuse, so only
 * wordcount_longest_line() may enter candidate work. */

#ifndef Z23_WORDCOUNT_H
#define Z23_WORDCOUNT_H

#include <stddef.h>

struct wordcount_report {
    size_t lines;
    size_t words;
    size_t bytes;
    size_t longest_line;
};

/* Length in bytes of the longest line in [text, text+len), excluding its
 * newline. THE MISSING BEHAVIOR: declared here, deliberately not defined in
 * src/, and required by tests/. The journey creates exactly this. */
size_t wordcount_longest_line(const char *text, size_t len);

/* Fill every field of the report. Reused counters plus the created one. */
void wordcount_measure(const char *text, size_t len,
                       struct wordcount_report *out);

#endif /* Z23_WORDCOUNT_H */
