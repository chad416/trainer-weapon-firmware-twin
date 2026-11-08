#include "control.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "cfg_store.h"
#include "imu_task.h"
#include "lane.h"
#include "malfunction.h"
#include "outputs.h"
#include "strobe_task.h"
#include "trigger_task.h"
#include "wdt.h"

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

#define CONTROL_STACK_SIZE     2560
#define CONTROL_PRIO           3
#define CONTROL_WAIT           K_MSEC(100) /* also the watchdog feed period */
#define CONTROL_WDT_TIMEOUT_MS 1000U

K_MSGQ_DEFINE(event_q, sizeof(struct weapon_event), CONFIG_TRAINER_EVENT_QUEUE_LEN, 4);
K_MUTEX_DEFINE(weapon_lock); /* guards weapon, malf and shots */
K_THREAD_STACK_DEFINE(control_stack, CONTROL_STACK_SIZE);

static struct k_thread control_thread;
static struct weapon weapon;
static struct malf malf;
static struct shotlog shots; /* 16 KB */
static uint32_t rng_state;
static atomic_t dropped;
static atomic_t stall_requested;

static void cadence_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)control_post(WEV_CADENCE_TICK);
}

K_TIMER_DEFINE(cadence_timer, cadence_expiry, NULL);

/* xorshift32: stoppage randomness for training, not security. */
static uint32_t next_random(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

int control_post(enum weapon_event_type type)
{
	struct weapon_event ev = {.type = type, .t_ms = k_uptime_get_32()};

	if (k_msgq_put(&event_q, &ev, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
		return -ENOMSG;
	}
	return 0;
}

void control_get_snapshot(struct control_snapshot *out)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	out->state = weapon_state(&weapon);
	out->sel = weapon.sel;
	out->rounds = weapon.rounds;
	out->shots = weapon.shots;
	out->last_clear_ms = weapon.last_clear_ms;
	k_mutex_unlock(&weapon_lock);
	out->dropped_events = (uint32_t)atomic_get(&dropped);
}

void control_debug_stall(void)
{
	atomic_set(&stall_requested, 1);
}

void control_shotlog_stats(uint16_t *count, uint32_t *total)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	*count = shotlog_count(&shots);
	*total = shotlog_total(&shots);
	k_mutex_unlock(&weapon_lock);
}

bool control_shotlog_get(uint16_t age, struct shot_record *out)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	bool ok = shotlog_get(&shots, age, out);

	k_mutex_unlock(&weapon_lock);
	return ok;
}

void control_shotlog_clear(void)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	shotlog_init(&shots);
	k_mutex_unlock(&weapon_lock);
}

uint8_t control_session_set(uint8_t key, uint32_t value)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	uint8_t st = malf_set_key(&malf, key, value);

	k_mutex_unlock(&weapon_lock);
	if (st == 0U) {
		LOG_INF("SESSION key=%u value=%u", key, value);
	}
	return st;
}

uint8_t control_session_get(uint8_t key, uint32_t *value)
{
	k_mutex_lock(&weapon_lock, K_FOREVER);
	uint8_t st = malf_get_key(&malf, key, value);

	k_mutex_unlock(&weapon_lock);
	return st;
}

/*
 * Before the state machine sees a trigger break in READY, or a cadence tick in
 * AUTO, the round may be turned into a stoppage: it does not fire, and the
 * weapon enters STOPPAGE exactly as if the instructor had injected one.
 */
static void maybe_stoppage(struct weapon_event *ev)
{
	enum weapon_state_id st = weapon_state(&weapon);
	bool round_attempt = ((ev->type == WEV_TRIGGER_BREAK) && (st == WS_READY)) ||
			     ((ev->type == WEV_CADENCE_TICK) && (st == WS_FIRING) &&
			      (weapon.sel == SEL_AUTO) && weapon.rounds.chambered);

	if (round_attempt && malf_on_round(&malf, next_random())) {
		ev->type = WEV_STOPPAGE_INJECT;
	}
}

/* Input events are reported as they happen, whatever the weapon did with them. */
static void report_input(const struct weapon_event *ev, const struct control_snapshot *s,
			 uint32_t t_us)
{
	switch (ev->type) {
	case WEV_MAG_TOGGLE:
		lane_report_event(s->rounds.mag_in ? PROTO_EV_MAG_IN : PROTO_EV_MAG_OUT,
				  rounds_total(&s->rounds), t_us);
		break;
	case WEV_RACK:
		lane_report_event(PROTO_EV_RACK, rounds_total(&s->rounds), t_us);
		break;
	case WEV_TAP:
		lane_report_event(PROTO_EV_TAP, 0U, t_us);
		break;
	default:
		break;
	}
}

static void report_detail(uint16_t shot_no, uint64_t local_us, struct proto_shot_detail *out)
{
	struct trigger_snapshot t;
	struct imu_shot_metrics m;

	trigger_get_snapshot(&t);
	(void)imu_shot_metrics((uint32_t)local_us, &m); /* zeros if the IMU is absent */

	struct proto_shot_detail d = {
		.shot_no = shot_no,
		.takeup_4ms = proto_sat_u8((t.last.takeup_ms + 2) / 4),
		.squeeze_4ms = proto_sat_u8((t.last.squeeze_ms + 2) / 4),
		.jerk_pm_per_ms = proto_sat_u8(t.last.jerk_pm_per_ms),
		.muzzle_rate_dps = proto_sat_u8((int32_t)(m.muzzle_rate_dps + 0.5f)),
		.cant_deg = proto_sat_i8((int32_t)((m.cant_deg >= 0.0f) ? (m.cant_deg + 0.5f)
									: (m.cant_deg - 0.5f))),
		.flinch_ddeg = proto_sat_i8((int32_t)((m.flinch_deg >= 0.0f)
							  ? ((m.flinch_deg * 10.0f) + 0.5f)
							  : ((m.flinch_deg * 10.0f) - 0.5f))),
	};

	lane_report_shot_detail(&d);
	*out = d;
	LOG_INF("DETAIL n=%u takeup=%ums squeeze=%ums jerk=%u rate=%u cant=%d flinch_ddeg=%d "
		"imu_n=%u",
		shot_no, t.last.takeup_ms, t.last.squeeze_ms, d.jerk_pm_per_ms, d.muzzle_rate_dps,
		d.cant_deg, d.flinch_ddeg, m.samples);
}

static void apply_actions(uint32_t act, const struct control_snapshot *s,
			  enum weapon_selector sel_before, uint64_t local_us, bool clear_late)
{
	uint32_t t_us = lane_time_of(local_us);

	if (act & WACT_FIRE) {
		if (strobe_active()) {
			/* Camera mode (F17): recoil now, the emitter in the shot frame. */
			outputs_valve();
			strobe_shot(lane_time64_of(local_us));
		} else {
			outputs_shot();
		}
		lane_report_shot((uint16_t)s->shots, t_us, (uint8_t)s->sel, s->rounds.chambered,
				 rounds_total(&s->rounds));
		/* t_local: exact shot time in us; log timestamps only have ms resolution. */
		LOG_INF("SHOT n=%u rounds=%u sel=%s t_local=%u", s->shots, rounds_total(&s->rounds),
			weapon_selector_name(s->sel), (uint32_t)local_us);

		struct proto_shot_detail d;

		report_detail((uint16_t)s->shots, local_us, &d);

		struct shot_record r = {
			.t_lane_us = t_us,
			.shot_no = (uint16_t)s->shots,
			.mode = (uint8_t)s->sel,
			.rounds_left = rounds_total(&s->rounds),
			.takeup_4ms = d.takeup_4ms,
			.squeeze_4ms = d.squeeze_4ms,
			.jerk_pm_per_ms = d.jerk_pm_per_ms,
			.muzzle_rate_dps = d.muzzle_rate_dps,
			.cant_deg = d.cant_deg,
			.flinch_ddeg = d.flinch_ddeg,
		};

		k_mutex_lock(&weapon_lock, K_FOREVER);
		shotlog_add(&shots, &r);
		k_mutex_unlock(&weapon_lock);
	}
	if (act & WACT_DRY_FIRE) {
		lane_report_event(PROTO_EV_DRY_FIRE, 0U, t_us);
		LOG_INF("DRY_FIRE");
	}
	if (act & WACT_CADENCE_STOP) {
		k_timer_stop(&cadence_timer);
	}
	if (act & WACT_CADENCE_START) {
		uint32_t period_us = 60000000U / cfg_live_auto_rpm();

		k_timer_start(&cadence_timer, K_USEC(period_us), K_USEC(period_us));
	}
	if (act & WACT_STOPPAGE_START) {
		lane_report_event(PROTO_EV_STOPPAGE_START, 0U, t_us);
		LOG_INF("STOPPAGE");
	}
	if (act & WACT_STOPPAGE_CLEAR) {
		uint16_t ms = (uint16_t)MIN(s->last_clear_ms, UINT16_MAX);

		lane_report_event(PROTO_EV_STOPPAGE_CLEARED, ms, t_us);
		LOG_INF("STOPPAGE_CLEARED t=%ums", s->last_clear_ms);
		if (clear_late) {
			lane_report_event(PROTO_EV_CLEAR_LATE, ms, t_us);
			LOG_INF("CLEAR_LATE t=%ums", s->last_clear_ms);
		}
	}
	if (s->sel != sel_before) {
		lane_report_event(PROTO_EV_SELECTOR, (uint16_t)s->sel, t_us);
		LOG_INF("SELECTOR %s", weapon_selector_name(s->sel));
	}
	if (act & WACT_STATE_CHANGED) {
		outputs_show_state(s->state);
		lane_report_event(PROTO_EV_STATE, (uint16_t)s->state, t_us);
		LOG_INF("STATE %s sel=%s mag=%s rounds=%u", weapon_state_name(s->state),
			weapon_selector_name(s->sel), s->rounds.mag_in ? "in" : "out",
			rounds_total(&s->rounds));
	}
}

static void control_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct weapon_event ev;
	struct control_snapshot snap;
	int wdt = wdt_register("control", CONTROL_WDT_TIMEOUT_MS);

	k_mutex_lock(&weapon_lock, K_FOREVER);
	weapon_init(&weapon, cfg_live_capacity());
	malf_init(&malf);
	shotlog_init(&shots);
	rng_state = k_cycle_get_32() | 1U; /* xorshift state must not be zero */
	k_mutex_unlock(&weapon_lock);

	outputs_show_state(WS_SAFE);
	LOG_INF("READY state=SAFE capacity=%u rpm=%u", cfg_live_capacity(), cfg_live_auto_rpm());

	for (;;) {
		wdt_feed(wdt);
		if (atomic_get(&stall_requested) != 0) {
			LOG_WRN("control thread stalling (debug command)");
			k_sleep(K_FOREVER);
		}
		if (k_msgq_get(&event_q, &ev, CONTROL_WAIT) != 0) {
			continue;
		}

		k_mutex_lock(&weapon_lock, K_FOREVER);
		enum weapon_selector sel_before = weapon.sel;

		/* Capacity changes apply to the next magazine inserted. */
		weapon.rounds.capacity = cfg_live_capacity();
		maybe_stoppage(&ev);

		uint32_t act = weapon_handle(&weapon, &ev);

		if ((ev.type == WEV_MAG_TOGGLE) && weapon.rounds.mag_in) {
			malf_on_mag_insert(&malf);
		}
		bool clear_late = ((act & WACT_STOPPAGE_CLEAR) != 0U) &&
				  malf_clear_late(&malf, weapon.last_clear_ms);
		k_mutex_unlock(&weapon_lock);

		uint64_t local_us = lane_local_us();

		control_get_snapshot(&snap);
		apply_actions(act, &snap, sel_before, local_us, clear_late);
		report_input(&ev, &snap, lane_time_of(local_us));

		/* Defensive: a late cadence tick must never keep the timer alive. */
		if ((ev.type == WEV_CADENCE_TICK) && (snap.state != WS_FIRING)) {
			k_timer_stop(&cadence_timer);
		}
	}
}

void control_start(void)
{
	(void)k_thread_create(&control_thread, control_stack, K_THREAD_STACK_SIZEOF(control_stack),
			      control_main, NULL, NULL, NULL, CONTROL_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&control_thread, "control");
}
