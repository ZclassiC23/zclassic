/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "util/result.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct zcl_result zcl_result_make(int code, const char *file, int line,
                                   const char *fmt, ...)
{
    struct zcl_result r = {
        .ok          = false,
        .code        = code,
        .source_file = file,
        .source_line = line,
    };
    r.message[0] = '\0';

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r.message, sizeof(r.message), fmt ? fmt : "", ap);
    va_end(ap);

    if (n < 0) {
        /* Formatting failure: degrade to a safe marker so callers
         * still see a non-empty message. */
        const char *fallback = "<zcl_result: vsnprintf failed>";
        size_t flen = strlen(fallback);
        if (flen >= sizeof(r.message)) flen = sizeof(r.message) - 1;
        memcpy(r.message, fallback, flen);
        r.message[flen] = '\0';
    } else if ((size_t)n >= sizeof(r.message)) {
        /* Truncation: keep the leading bytes vsnprintf wrote but make
         * the truncation visible so operators aren't misled by a
         * silently cut error string. */
        const char marker[] = "...[truncated]";
        size_t keep = sizeof(r.message) - sizeof(marker);
        memcpy(r.message + keep, marker, sizeof(marker));
    }

    return r;
}
