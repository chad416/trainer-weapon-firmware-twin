#include "lane.h"

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/reboot.h>

#include "bit.h"
#include "canlink.h"
#include "cfg_store.h"
#include "control.h"
#include "local_clock.h"
#include "strobe_task.h"
#include "timesync.h"
#include "wdt.h"

LOG_MODULE_REGISTER(lane, LOG_LEVEL_INF);

#define LANE_STACK_SIZE     2048
#define LANE_PRIO           6
#define STATUS_PERIOD_MS    100U
#define LANE_WDT_TIMEOUT_MS 2000U /* a SET writes flash, allow for it */
#define KEY_BOOT_COUNT      8U
#define KEY_SESSION_FIRST   9U
#define KEY_SESSION_LAST    12U

K_THREAD_STACK_DEFINE(lane_stack, LANE_STACK_SIZE);

static struct k_thread lane_thread;
static uint8_t node;
static struct timesync ts;
static struct k_spinlock ts_lock;
static bool ts_ready;

/* ---- time -------------------------------------------------------------------- */

uint64_t lane_local_us(void)
{
	return local_clock_us();
}

uint32_t lane_time_of(uint64_t local_us)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	uint32_t t = ts_ready ? ts_lane_time(&ts, local_us) : (uint32_t)local_us;

	k_spin_unlock(&ts_lock, key);
	return t;
}

uint32_t lane_now_us(void)
{
	return lane_time_of(lane_local_us());
}

int64_t lane_time64_of(uint64_t local_us)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	int64_t t = ts_ready ? ts_lane_time64(&ts, local_us) : (int64_t)local_us;

	k_spin_unlock(&ts_lock, key);
	return t;
}

uint64_t lane_local_of(int64_t lane_us)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	uint64_t t = ts_ready ? ts_local_time(&ts, lane_us) : (uint64_t)lane_us;

	k_spin_unlock(&ts_lock, key);
	return t;
}

bool lane_synced(void)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	bool s = ts_ready && ts.synced;

	k_spin_unlock(&ts_lock, key);
	return s;
}

void lane_get_sync_info(struct lane_sync_info *out)
{
	uint64_t now = lane_local_us();
	k_spinlock_key_t key = k_spin_lock(&ts_lock);

	out->synced = ts_ready && ts.synced;
	out->offset_us = out->synced ? ts_offset_us(&ts, now) : 0;
	out->drift_ppb = out->synced ? (int32_t)(ts_drift_ppm(&ts) * 1000.0) : 0;
	out->updates = ts.updates;
	out->resets = ts.resets;
	k_spin_unlock(&ts_lock, key);
}

static void add_sync(const struct canlink_sync *p)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	bool first = !ts.synced;

	ts_add(&ts, p->local_us, p->lane_us);
	k_spin_unlock(&ts_lock, key);
	if (first) {
		LOG_INF("SYNC locked");
	}
}

/* ---- reports ------------------------------------------------------------------ */

static void send(uint8_t function, const uint8_t *data, uint8_t len)
{
	(void)canlink_send(proto_id(function, node), data, len);
}

void lane_report_shot(uint16_t shot_no, uint32_t t_us, uint8_t mode, bool chambered,
		      uint8_t rounds_left)
{
	struct proto_shot_event m = {.shot_no = shot_no, .t_us = t_us, .mode = mode,
				     .chambered = chambered, .rounds_left = rounds_left};
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_SHOT_EVENT, d, proto_encode_shot_event(&m, d));
}

void lane_report_shot_detail(const struct proto_shot_detail *m)
{
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_SHOT_DETAIL, d, proto_encode_shot_detail(m, d));
}

void lane_report_event(enum proto_event_type type, uint16_t value, uint32_t t_us)
{
	struct proto_event m = {.type = (uint8_t)type, .t_us = t_us, .value = value};
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_EVENT, d, proto_encode_event(&m, d));
}

void lane_report_bit(uint16_t mask, bool critical)
{
	struct proto_bit_result m = {.fault_mask = mask, .critical = critical};
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_BIT_RESULT, d, proto_encode_bit_result(&m, d));
}

void lane_send_info(void)
{
	struct proto_info m = {
		.ver_major = APP_VERSION_MAJOR,
		.ver_minor = APP_VERSION_MINOR,
		.ver_patch = APP_PATCHLEVEL,
		.reset_reason = (uint8_t)wdt_reset_reason(),
		.boot_count = cfg_store_boot_count(),
	};
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_INFO, d, proto_encode_info(&m, d));
}

static void send_status(void)
{
	struct control_snapshot s;
	struct canlink_stats c;

	control_get_snapshot(&s);
	canlink_get_stats(&c);

	struct proto_status m = {
		.state = (uint8_t)s.state,
		.sel = (uint8_t)s.sel,
		.mag_in = s.rounds.mag_in,
		.chambered = s.rounds.chambered,
		.stoppage = (s.state == WS_STOPPAGE),
		.rounds = rounds_total(&s.rounds),
		.faults = bit_last_mask(),
		.tx_overflows = (uint8_t)MIN(c.tx_overflows, UINT8_MAX),
		.tx_errors = (uint8_t)MIN(c.tx_errors, PROTO_TX_ERRORS_MAX),
		.shots = (uint16_t)s.shots,
	};
	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_STATUS, d, proto_encode_status(&m, d));
}

/* ---- commands ----------------------------------------------------------------- */

static bool is_session_key(uint8_t key)
{
	return (key >= KEY_SESSION_FIRST) && (key <= KEY_SESSION_LAST);
}

static void handle_cmd(const struct can_frame *f)
{
	struct proto_cmd c;

	if (!proto_decode_cmd(f->data, can_dlc_to_bytes(f->dlc), &c)) {
		LOG_WRN("malformed CMD ignored");
		return;
	}

	struct proto_cmd_ack a = {.opcode = c.opcode, .key = c.key, .status = PROTO_ST_OK,
				  .value = 0U};
	bool reboot = false;

	switch (c.opcode) {
	case PROTO_OP_GET:
		if (c.key == KEY_BOOT_COUNT) {
			a.value = cfg_store_boot_count();
		} else if (strobe_is_key(c.key)) {
			a.status = strobe_session_get(c.key, &a.value);
		} else if (is_session_key(c.key)) {
			a.status = control_session_get(c.key, &a.value);
		} else {
			a.status = cfg_store_get_key(c.key, &a.value);
		}
		break;
	case PROTO_OP_SET:
		if (c.key == KEY_BOOT_COUNT) {
			a.status = PROTO_ST_READ_ONLY;
		} else if (strobe_is_key(c.key)) {
			a.status = strobe_session_set(c.key, c.value);
		} else if (is_session_key(c.key)) {
			a.status = control_session_set(c.key, c.value);
		} else {
			a.status = cfg_store_set(c.key, c.value);
		}
		a.value = c.value;
		break;
	case PROTO_OP_RUN_BIT:
		a.value = bit_run();
		break;
	case PROTO_OP_INJECT_STOPPAGE:
		(void)control_post(WEV_STOPPAGE_INJECT);
		break;
	case PROTO_OP_REBOOT:
		reboot = true;
		break;
	case PROTO_OP_RESET_DEFAULTS:
		a.status = cfg_store_reset_defaults();
		break;
	default:
		a.status = PROTO_ST_BAD_OPCODE;
		break;
	}

	uint8_t d[PROTO_FRAME_LEN];

	send(PROTO_FN_CMD_ACK, d, proto_encode_cmd_ack(&a, d));
	LOG_INF("CMD op=%u key=%u value=%u -> status=%u", c.opcode, c.key, c.value, a.status);

	if (reboot) {
		LOG_INF("REBOOT requested");
		k_sleep(K_MSEC(50)); /* let the ACK and the log go out */
		sys_reboot(SYS_REBOOT_COLD);
	}
}

/* ---- thread ------------------------------------------------------------------- */

static void lane_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct k_msgq *cmd_q = canlink_cmd_queue();
	struct k_msgq *sync_q = canlink_sync_queue();
	struct k_poll_event events[2] = {
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					 cmd_q),
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					 sync_q),
	};
	int wdt = wdt_register("lane", LANE_WDT_TIMEOUT_MS);
	int64_t next_status = k_uptime_get() + STATUS_PERIOD_MS;
	struct canlink_sync p;
	struct can_frame f;

	for (;;) {
		wdt_feed(wdt);

		int64_t wait = next_status - k_uptime_get();

		if (wait > 0) {
			(void)k_poll(events, ARRAY_SIZE(events), K_MSEC(wait));
		}
		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;

		while (k_msgq_get(sync_q, &p, K_NO_WAIT) == 0) {
			add_sync(&p);
		}
		while (k_msgq_get(cmd_q, &f, K_NO_WAIT) == 0) {
			handle_cmd(&f);
		}

		int64_t now = k_uptime_get();

		if (now >= next_status) {
			send_status();
			next_status += STATUS_PERIOD_MS;
			if (next_status <= now) {
				next_status = now + STATUS_PERIOD_MS; /* fell behind: no burst */
			}
		}
	}
}

void lane_start(uint8_t node_id)
{
	node = node_id;
	ts_init(&ts);
	ts_ready = true;
	(void)k_thread_create(&lane_thread, lane_stack, K_THREAD_STACK_SIZEOF(lane_stack),
			      lane_main, NULL, NULL, NULL, LANE_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&lane_thread, "lane");
}
