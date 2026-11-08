/*
 * Local monotonic time in microseconds, from the 64-bit CPU cycle counter
 * (v0.6.1). It counts from the same origin as the kernel tick, so tick N
 * starts at local time N * 100 us. The SYNC timestamps, lane time and the
 * strobe all use it: the tick clock (k_uptime_ticks) is only 100 us fine, and
 * flooring every SYNC timestamp to it put the lane-time estimate up to 100 us
 * off, which the emitter pulses inherited.
 */
#ifndef TRAINER_LOCAL_CLOCK_H_
#define TRAINER_LOCAL_CLOCK_H_

#include <stdint.h>
#include <zephyr/kernel.h>

static inline uint64_t local_clock_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

#endif /* TRAINER_LOCAL_CLOCK_H_ */
