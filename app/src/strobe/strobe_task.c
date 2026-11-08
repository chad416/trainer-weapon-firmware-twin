#include "strobe_task.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "lane.h"
#include "outputs.h"
#include "proto.h"
#include "strobe.h"
#include "wdt.h"

LOG_MODULE_REGISTER(strobe, LOG_LEVEL_INF);

#define STROBE_STACK_SIZE     1024
#define STROBE_PRIO           2 /* above control (3): pulses are time-critical */
#define STROBE_IDLE           K_MSEC(100)
#define STROBE_MAX_WAIT_US    100000 /* re-plan at least this often (watchdog, new SYNC) */
#define STROBE_WDT_TIMEOUT_MS 1000U
#define STROBE_LEAD_US        300 /* plan pulses at least this far ahead */
#define STROBE_ARM_TICKS      2   /* wake this many ticks before the on edge to arm the window */

K_THREAD_STACK_DEFINE(strobe_stack, STROBE_STACK_SIZE);
K_SEM_DEFINE(strobe_wake, 0, 1);

static struct k_thread strobe_thread;
static struct k_spinlock lock;
static struct strobe_cfg cfg;           /* guarded by lock */
static int64_t shot_k = STROBE_NO_SHOT; /* guarded by lock */
static struct strobe_stats stats;       /* guarded by lock */
static int64_t max_err_us;              /* guarded by lock */

bool strobe_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool on = cfg.n_slots != 0U;

	k_spin_unlock(&lock, key);
	return on;
}

void strobe_shot(int64_t t_break_lane_us)
{
	int64_t k = strobe_shot_frame(t_break_lane_us);
	k_spinlock_key_t key = k_spin_lock(&lock);

	shot_k = k;
	stats.last_shot_k = k;
	k_spin_unlock(&lock, key);
	k_sem_give(&strobe_wake);
}

bool strobe_is_key(uint8_t key)
{
	return (key == STROBE_KEY_SLOTS) || (key == STROBE_KEY_SLOT);
}

uint8_t strobe_session_set(uint8_t key, uint32_t value)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	struct strobe_cfg c = cfg;
	uint8_t st = PROTO_ST_OK;

	if (value > 255U) {
		st = PROTO_ST_BAD_VALUE;
	} else if (key == STROBE_KEY_SLOTS) {
		c.n_slots = (uint8_t)value;
	} else if (key == STROBE_KEY_SLOT) {
		c.slot = (uint8_t)value;
	} else {
		st = PROTO_ST_BAD_KEY;
	}
	if ((st == PROTO_ST_OK) && !strobe_cfg_ok(c.n_slots, c.slot)) {
		st = PROTO_ST_BAD_VALUE;
	}
	if (st == PROTO_ST_OK) {
		cfg = c;
		shot_k = STROBE_NO_SHOT;
		max_err_us = 0; /* the worst error is kept per camera-mode setting */
	}
	k_spin_unlock(&lock, k);
	if (st == PROTO_ST_OK) {
		LOG_INF("STROBE slots=%u slot=%u", c.n_slots, c.slot);
		k_sem_give(&strobe_wake);
	}
	return st;
}

uint8_t strobe_session_get(uint8_t key, uint32_t *value)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	uint8_t st = PROTO_ST_OK;

	if (key == STROBE_KEY_SLOTS) {
		*value = cfg.n_slots;
	} else if (key == STROBE_KEY_SLOT) {
		*value = cfg.slot;
	} else {
		st = PROTO_ST_BAD_KEY;
	}
	k_spin_unlock(&lock, k);
	return st;
}

void strobe_get_stats(struct strobe_stats *out)
{
	k_spinlock_key_t k = k_spin_lock(&lock);

	*out = stats;
	out->n_slots = cfg.n_slots;
	out->slot = cfg.slot;
	out->max_err_us = max_err_us;
	k_spin_unlock(&lock, k);
	out->synced = lane_synced();
}

/*
 * The emitter window for a pulse starting at system tick on: 0.5 ms, both
 * edges on tick edges (outputs_emitter_window). If the thread woke too late
 * for that tick, the window moves to the next tick edge (counted in max_err).
 * A pulse without lane time (planned = false) has no planned start, so it is
 * counted in unsynced and kept out of the timing error statistics.
 */
static void fire(const struct strobe_pulse *p, int64_t planned_shot, k_ticks_t on, bool planned)
{
	k_ticks_t now_tick = k_uptime_ticks();

	if (on <= now_tick) {
		on = now_tick + 1;
	}

	uint64_t on_cycles = outputs_emitter_window(on, on + k_us_to_ticks_near64(STROBE_PULSE_US));
	uint64_t local = k_cyc_to_us_floor64(on_cycles);
	int64_t lane = lane_time64_of(local);
	int64_t err = lane - p->start_us;
	k_spinlock_key_t k = k_spin_lock(&lock);

	stats.pulses++;
	stats.last_kind = p->kind;
	stats.last_k = p->k;
	stats.last_lane_us = lane;
	stats.last_local_us = local;
	if (planned) {
		stats.last_err_us = err;
		if (((err < 0) ? -err : err) > max_err_us) {
			max_err_us = (err < 0) ? -err : err;
		}
	} else {
		stats.unsynced++;
	}
	if (p->kind == STROBE_SHOT) {
		stats.shots++;
		stats.last_shot_lane_us = lane;
		stats.last_shot_fired_k = p->k;
		if (p->late) {
			stats.late++;
		}
		if (shot_k == planned_shot) {
			shot_k = STROBE_NO_SHOT; /* a newer break may have replaced it meanwhile */
		}
	} else {
		stats.aim++;
	}
	k_spin_unlock(&lock, k);
}

static void strobe_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = wdt_register("strobe", STROBE_WDT_TIMEOUT_MS);

	for (;;) {
		wdt_feed(wdt);

		k_spinlock_key_t k = k_spin_lock(&lock);
		struct strobe_cfg c = cfg;
		int64_t sk = shot_k;

		k_spin_unlock(&lock, k);
		if ((c.n_slots == 0U) || !lane_synced()) {
			if ((c.n_slots != 0U) && (sk != STROBE_NO_SHOT)) {
				/* Camera mode without lane time: the shot cannot be placed. Flash now. */
				struct strobe_pulse p = {.k = sk, .start_us = 0, .kind = STROBE_SHOT, .late = false};

				fire(&p, sk, k_uptime_ticks() + 1, false);
			}
			(void)k_sem_take(&strobe_wake, STROBE_IDLE);
			continue;
		}

		uint64_t now = lane_local_us();
		struct strobe_pulse p;

		if (!strobe_next(&c, lane_time64_of(now) + STROBE_LEAD_US, sk, &p)) {
			(void)k_sem_take(&strobe_wake, STROBE_IDLE);
			continue;
		}
		uint64_t start_local = lane_local_of(p.start_us);

		if ((int64_t)(start_local - now) > STROBE_MAX_WAIT_US) {
			(void)k_sem_take(&strobe_wake, K_USEC(STROBE_MAX_WAIT_US));
			continue; /* far ahead: re-plan later */
		}
		/* The tick edge nearest the planned start: within half a tick (50 us). */
		k_ticks_t on = (k_ticks_t)k_us_to_ticks_near64(start_local);

		if ((k_uptime_ticks() < (on - STROBE_ARM_TICKS)) &&
		    (k_sem_take(&strobe_wake, K_TIMEOUT_ABS_TICKS(on - STROBE_ARM_TICKS)) == 0)) {
			continue; /* new shot or new settings: re-plan */
		}
		fire(&p, sk, on, true);
	}
}

void strobe_start(void)
{
	(void)k_thread_create(&strobe_thread, strobe_stack, K_THREAD_STACK_SIZEOF(strobe_stack),
			      strobe_main, NULL, NULL, NULL, STROBE_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&strobe_thread, "strobe");
}
