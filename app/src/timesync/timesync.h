/*
 * Lane time synchronisation. SPEC F12.
 *
 * The lane broadcasts SYNC frames carrying its microsecond clock (32 bit). The
 * node timestamps each one on reception with its own clock and fits
 *     lane = local0' + slope * (local - local0)
 * by least squares over the last TS_WINDOW points, which gives both offset
 * and drift. A jump larger than TS_JUMP_US (lane restarted) restarts the fit.
 *
 * Pure logic: unit-tested on native_sim.
 */
#ifndef TRAINER_TIMESYNC_H_
#define TRAINER_TIMESYNC_H_

#include <stdbool.h>
#include <stdint.h>

#define TS_WINDOW  32U /* 310 ms of SYNC at 10 ms: ~5 ppm slope noise with 3 us jitter */
#define TS_JUMP_US 10000
#define TS_MAX_DRIFT 500e-6 /* slope bound: +-500 ppm */

struct timesync {
	uint64_t local[TS_WINDOW];
	int64_t lane[TS_WINDOW]; /* unwrapped 64-bit lane time */
	uint8_t n;
	uint8_t head;
	uint32_t last_lane32;
	int64_t lane_epoch; /* adds 2^32 per wrap of the 32-bit lane clock */
	/* fit */
	bool synced;
	uint64_t ref_local;
	double ref_lane;
	double slope;
	uint32_t updates;
	uint32_t resets;
};

void ts_init(struct timesync *ts);

/* Add a SYNC point: local reception time and the lane time it carried. */
void ts_add(struct timesync *ts, uint64_t local_us, uint32_t lane_us);

/* Lane time for a local time. Before the first SYNC this is the local time. */
uint32_t ts_lane_time(const struct timesync *ts, uint64_t local_us);

/* Current estimates, for diagnostics: offset at local_us and drift in ppm. */
int64_t ts_offset_us(const struct timesync *ts, uint64_t local_us);
double ts_drift_ppm(const struct timesync *ts);

#endif /* TRAINER_TIMESYNC_H_ */
