/*
 * Lane protocol on top of canlink: reports (SHOT_EVENT, SHOT_DETAIL, EVENT,
 * STATUS, BIT_RESULT, INFO), the CMD/CMD_ACK service, and lane time from SYNC.
 * SPEC §6, F12.
 */
#ifndef TRAINER_LANE_H_
#define TRAINER_LANE_H_

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

/* Start the lane thread (CMD service, SYNC fit, 10 Hz STATUS). canlink must be up. */
void lane_start(uint8_t node_id);

void lane_report_shot(uint16_t shot_no, uint32_t t_us, uint8_t mode, bool chambered,
		      uint8_t rounds_left);
void lane_report_shot_detail(const struct proto_shot_detail *d);
void lane_report_event(enum proto_event_type type, uint16_t value, uint32_t t_us);
void lane_report_bit(uint16_t mask, bool critical);
void lane_send_info(void);

/* Local monotonic time in microseconds (k_uptime clock). */
uint64_t lane_local_us(void);

/* Lane time for a local time: synchronised once SYNC frames arrive, else local. */
uint32_t lane_time_of(uint64_t local_us);

/* Lane time now. */
uint32_t lane_now_us(void);

/* 64-bit (unwrapped) lane time for a local time, its inverse, and whether SYNC has locked (v0.6). */
int64_t lane_time64_of(uint64_t local_us);
uint64_t lane_local_of(int64_t lane_us);
bool lane_synced(void);

struct lane_sync_info {
	bool synced;
	int64_t offset_us;
	int32_t drift_ppb; /* parts per billion */
	uint32_t updates;
	uint32_t resets;
};

void lane_get_sync_info(struct lane_sync_info *out);

#endif /* TRAINER_LANE_H_ */
