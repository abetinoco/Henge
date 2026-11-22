#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "pvg_vblank_clock.h"

#define TICKS_PER_SECOND 24000000ull

/* Polling at 1 ms is deliberately incommensurate with 30/60/120 Hz.
   Compare event counts against the requested frequency, not helper internals. */
static void cadence(unsigned hz) {
    HvVblankClock clock = {0};
    uint64_t period = TICKS_PER_SECOND / hz, old_next = 0;
    unsigned events = 0, old_events = 0;
    for (uint64_t now = 0; now < 10 * TICKS_PER_SECOND; now += TICKS_PER_SECOND / 1000) {
        events += hv_vblank_due(&clock, now, period);
        if (now >= old_next) { old_events++; old_next = now + period; }
    }
    assert(events == 10 * hz);
    assert(old_events < events);
    printf("%u Hz, 10 s of 1 ms polls: old %u events, phase-preserving %u\n", hz, old_events, events);
}

static void boundary_and_stall(void) {
    HvVblankClock clock = {0};
    uint64_t period = TICKS_PER_SECOND / 60;
    assert(hv_vblank_due(&clock, 17, period));
    assert(!hv_vblank_due(&clock, 17 + period - 1, period));
    assert(hv_vblank_due(&clock, 17 + period, period));
    assert(!hv_vblank_due(&clock, 17 + period, period));
    /* One late event after an hour, then silence until the next anchored slot. */
    uint64_t late = 17 + 3600 * TICKS_PER_SECOND + period / 2;
    assert(hv_vblank_due(&clock, late, period));
    assert(!hv_vblank_due(&clock, late, period));
    uint64_t expected = 17 + 3600 * TICKS_PER_SECOND + period;
    assert(!hv_vblank_due(&clock, expected - 1, period));
    assert(hv_vblank_due(&clock, expected, period));
    assert(!hv_vblank_due(&clock, expected, period));
}

static void wrap_and_invalid_period(void) {
    HvVblankClock clock = {0};
    assert(!hv_vblank_due(&clock, 0, 0) && !clock.started);
    assert(!hv_vblank_due(&clock, 0, UINT64_MAX) && !clock.started);
    /* A 40-tick schedule begins 20 ticks before wrap: deadline wraps to 19. */
    assert(hv_vblank_due(&clock, UINT64_MAX - 20, 40));
    assert(!hv_vblank_due(&clock, UINT64_MAX - 1, 40));
    assert(!hv_vblank_due(&clock, 18, 40));
    assert(hv_vblank_due(&clock, 19, 40));
    assert(!hv_vblank_due(&clock, 19, 40));
    assert(!hv_vblank_due(&clock, 58, 40));
    assert(hv_vblank_due(&clock, 59, 40));
    /* A late poll across wrap skips missed periods, retaining the same phase. */
    assert(hv_vblank_due(&clock, 150, 40));
    assert(!hv_vblank_due(&clock, 178, 40));
    assert(hv_vblank_due(&clock, 179, 40));
}

int main(void) {
    cadence(30); cadence(60); cadence(120);
    boundary_and_stall();
    wrap_and_invalid_period();
    puts("vblank clock: cadence, exact deadlines, long stalls and counter wrap passed");
    return 0;
}
