/*
 * On-device shot log: ring buffer of the last SHOTLOG_CAPACITY shot records
 * (SPEC F15). RAM only; the oldest record is overwritten when full.
 *
 * Pure logic: unit-tested on native_sim. The caller provides locking.
 */
#ifndef TRAINER_SHOTLOG_H_
#define TRAINER_SHOTLOG_H_

#include <stdbool.h>
#include <stdint.h>

#define SHOTLOG_CAPACITY 1000U

/* One shot as reported on the bus: SHOT_EVENT plus SHOT_DETAIL. 16 bytes. */
struct shot_record {
	uint32_t t_lane_us;
	uint16_t shot_no;
	uint8_t mode; /* weapon_selector */
	uint8_t rounds_left;
	uint8_t takeup_4ms;
	uint8_t squeeze_4ms;
	uint8_t jerk_pm_per_ms;
	uint8_t muzzle_rate_dps;
	int8_t cant_deg;
	int8_t flinch_ddeg;
	uint8_t reserved[2];
};

struct shotlog {
	struct shot_record rec[SHOTLOG_CAPACITY];
	uint16_t head;  /* next write position */
	uint16_t count; /* records held, <= SHOTLOG_CAPACITY */
	uint32_t total; /* records ever added (count + overwritten) */
};

void shotlog_init(struct shotlog *log);
void shotlog_add(struct shotlog *log, const struct shot_record *r);

/* Records held now. */
uint16_t shotlog_count(const struct shotlog *log);

/* Records ever added, including those already overwritten. */
uint32_t shotlog_total(const struct shotlog *log);

/*
 * Record by age: age 0 is the newest, age count-1 the oldest held.
 * Returns false if age >= count.
 */
bool shotlog_get(const struct shotlog *log, uint16_t age, struct shot_record *out);

#endif /* TRAINER_SHOTLOG_H_ */
