#include "timesync.h"

#include <string.h>

#define LANE_WRAP (((int64_t)1) << 32)

void ts_init(struct timesync *ts)
{
	(void)memset(ts, 0, sizeof(*ts));
	ts->slope = 1.0;
}

/* Extend the 32-bit lane clock to 64 bits across wraps. */
static int64_t unwrap(struct timesync *ts, uint32_t lane_us)
{
	if ((ts->n > 0U) && (lane_us < ts->last_lane32) &&
	    ((ts->last_lane32 - lane_us) > 0x80000000U)) {
		ts->lane_epoch += LANE_WRAP;
	}
	ts->last_lane32 = lane_us;
	return ts->lane_epoch + (int64_t)lane_us;
}

static void fit(struct timesync *ts)
{
	/* Centre on the newest point to keep the doubles well conditioned. */
	uint8_t newest = (uint8_t)((ts->head + TS_WINDOW - 1U) % TS_WINDOW);
	uint64_t l0 = ts->local[newest];
	int64_t n0 = ts->lane[newest];
	double sx = 0.0;
	double sy = 0.0;

	for (uint8_t i = 0U; i < ts->n; i++) {
		sx += (double)(int64_t)(ts->local[i] - l0);
		sy += (double)(ts->lane[i] - n0);
	}
	double mx = sx / ts->n;
	double my = sy / ts->n;
	double sxx = 0.0;
	double sxy = 0.0;

	for (uint8_t i = 0U; i < ts->n; i++) {
		double dx = (double)(int64_t)(ts->local[i] - l0) - mx;
		double dy = (double)(ts->lane[i] - n0) - my;

		sxx += dx * dx;
		sxy += dx * dy;
	}

	ts->slope = (sxx > 0.0) ? (sxy / sxx) : 1.0;

	/*
	 * Real oscillators drift by tens of ppm. Badly jittered SYNC reception
	 * (overloaded bus, misbehaving lane master) can produce an absurd slope
	 * that would skew every timestamp, so bound it.
	 */
	if (ts->slope > (1.0 + TS_MAX_DRIFT)) {
		ts->slope = 1.0 + TS_MAX_DRIFT;
	} else if (ts->slope < (1.0 - TS_MAX_DRIFT)) {
		ts->slope = 1.0 - TS_MAX_DRIFT;
	}
	/* Line through the mean point, expressed at l0. */
	ts->ref_local = l0;
	ts->ref_lane = (double)n0 + my - (ts->slope * mx);
	ts->synced = true;
}

void ts_add(struct timesync *ts, uint64_t local_us, uint32_t lane_us)
{
	int64_t lane = unwrap(ts, lane_us);

	/* A lane restart or a clock step makes old points useless: start again. */
	if (ts->synced) {
		double predicted = ts->ref_lane +
				   (ts->slope * (double)(int64_t)(local_us - ts->ref_local));
		double err = (double)lane - predicted;

		if ((err > TS_JUMP_US) || (err < -TS_JUMP_US)) {
			ts->n = 0U;
			ts->head = 0U;
			ts->resets++;
		}
	}

	ts->local[ts->head] = local_us;
	ts->lane[ts->head] = lane;
	ts->head = (uint8_t)((ts->head + 1U) % TS_WINDOW);
	if (ts->n < TS_WINDOW) {
		ts->n++;
	}
	ts->updates++;
	fit(ts);
}

uint32_t ts_lane_time(const struct timesync *ts, uint64_t local_us)
{
	if (!ts->synced) {
		return (uint32_t)local_us;
	}
	double lane = ts->ref_lane + (ts->slope * (double)(int64_t)(local_us - ts->ref_local));

	return (uint32_t)(int64_t)(lane + 0.5);
}

int64_t ts_offset_us(const struct timesync *ts, uint64_t local_us)
{
	if (!ts->synced) {
		return 0;
	}
	double lane = ts->ref_lane + (ts->slope * (double)(int64_t)(local_us - ts->ref_local));

	return (int64_t)(lane + 0.5) - (int64_t)local_us;
}

double ts_drift_ppm(const struct timesync *ts)
{
	return (ts->slope - 1.0) * 1e6;
}
