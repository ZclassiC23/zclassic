/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Closed, receipt-bound compiler profile for portable Commons code. */

#ifndef ZCL_CONFIG_C23_COMMONS_BUILD_PROFILE_H
#define ZCL_CONFIG_C23_COMMONS_BUILD_PROFILE_H

/* V2 makes the original AMD64/SSE2 floor explicit. V1 receipts remain
 * distinguishable by their older flags string; no historical evidence is
 * relabeled. */
#define ZCL_C23_COMMONS_BUILD_TARGET_V2 "linux-x86_64"
#define ZCL_C23_COMMONS_BUILD_FLAGS_QUICK_V2 \
    "-std=c23 -O1 -march=x86-64 -mtune=generic -fno-omit-frame-pointer " \
    "-D_POSIX_C_SOURCE=200809L -ffile-prefix-map=SOURCE=. -c"
#define ZCL_C23_COMMONS_BUILD_FLAGS_STANDARD_V2 \
    "-std=c23 -O1 -march=x86-64 -mtune=generic -fno-omit-frame-pointer " \
    "-D_POSIX_C_SOURCE=200809L -ffile-prefix-map=SOURCE=. " \
    "-Wall -Wextra -Werror;asan,ubsan=clean;" \
    "sanitizer_pie=off;sanitizer_aslr=off"

#endif /* ZCL_CONFIG_C23_COMMONS_BUILD_PROFILE_H */
