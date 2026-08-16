/* zdogfight demo: run a fixed-seed match between two built-in trivial
 * pilots and print the final state root as hex. The root printed here
 * is the package's built-in FNV-1a/64 checksum of the canonical state
 * encoding (the library deliberately has no hash dependency; callers
 * that need a cryptographic root hash zdog_state_encode's bytes). */
#include "zdogfight/zdogfight.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Trivial pilot A: full throttle straight ahead, always firing. */
static void pilot_straight(zdog_ctl *c)
{
    c->roll = 0;
    c->pitch = 0;
    c->throttle = 32767;
    c->fire = 1;
}

/* Trivial pilot B: gentle weave, firing. */
static void pilot_weave(uint64_t tick, zdog_ctl *c)
{
    c->roll = (int16_t)((tick / 90u) % 2u ? 12000 : -12000);
    c->pitch = 0;
    c->throttle = 32767;
    c->fire = (uint8_t)(tick % 6u == 0u);
}

int main(int argc, char **argv)
{
    zdog_match m;

    if (argc < 2 || strcmp(argv[1], "selftest") != 0) {
        fprintf(stderr, "usage: zdogfight selftest\n");
        return 2;
    }
    zdog_match_init(&m, 42, 2);
    while (m.phase == ZDOG_PHASE_RUNNING) {
        zdog_ctl ctls[ZDOG_MAX_PLANES] = {{0}};

        for (unsigned i = 0; i < m.num_planes; i++) {
            if (m.planes[i].team == 0)
                pilot_straight(&ctls[i]);
            else
                pilot_weave(m.tick, &ctls[i]);
        }
        zdog_tick(&m, ctls);
    }
    printf("tick=%" PRIu64 " winner=%s score=%" PRIu32 "-%" PRIu32
           " root=%016" PRIx64 "\n",
           m.tick,
           m.winner == ZDOG_WINNER_RED    ? "red"
           : m.winner == ZDOG_WINNER_BLUE ? "blue"
                                          : "draw",
           m.score[0], m.score[1], zdog_state_checksum(&m));
    return 0;
}
