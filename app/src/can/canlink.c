#include "canlink.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "proto.h"
#include "wdt.h"

LOG_MODULE_REGISTER(canlink, LOG_LEVEL_INF);

#define CAN_BITRATE      500000U
#define TX_QUEUE_LEN     32
#define CMD_QUEUE_LEN    8
#define TX_STACK_SIZE    1024
#define TX_PRIO          4
#define TX_WAIT          K_MSEC(100)
#define TX_WDT_TIMEOUT_MS 1000U

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

K_MSGQ_DEFINE(tx_q, sizeof(struct can_frame), TX_QUEUE_LEN, 4);
K_MSGQ_DEFINE(cmd_q, sizeof(struct can_frame), CMD_QUEUE_LEN, 4);
K_MSGQ_DEFINE(sync_q, sizeof(struct canlink_sync), 16, 8);
K_THREAD_STACK_DEFINE(tx_stack, TX_STACK_SIZE);

static struct k_thread tx_thread;
static atomic_t tx_ok;
static atomic_t tx_errors;
static atomic_t tx_overflows;
static atomic_t bus_off_count;
static atomic_t rx_cmds;
static atomic_t rx_dropped;
static atomic_t rx_syncs;
static atomic_t bus_state = ATOMIC_INIT(CAN_STATE_STOPPED);
static bool link_up;

static struct can_state_change_callback state_cb;

static void state_changed(const struct device *dev, struct can_state_change_callback *cb,
			  enum can_state state, struct can_bus_err_cnt err_cnt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(err_cnt);

	atomic_set(&bus_state, state);
	if (state == CAN_STATE_BUS_OFF) {
		/* Zephyr recovers automatically (CONFIG_CAN_MANUAL_RECOVERY_MODE=n). */
		atomic_inc(&bus_off_count);
	}
}

/* RX filter callback (ISR): count every CMD frame the controller delivers. */
static void cmd_received(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	atomic_inc(&rx_cmds);
	if (k_msgq_put(&cmd_q, frame, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

/*
 * RX filter callback (ISR) for SYNC. The local timestamp is taken here, as
 * close to the reception as software can get; the fit runs later in the lane
 * thread (no floating point in ISRs).
 */
static void sync_received(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	struct canlink_sync p = {.local_us = k_ticks_to_us_floor64(k_uptime_ticks())};

	if (proto_decode_sync(frame->data, can_dlc_to_bytes(frame->dlc), &p.lane_us)) {
		atomic_inc(&rx_syncs);
		(void)k_msgq_put(&sync_q, &p, K_NO_WAIT); /* a lost SYNC is harmless */
	}
}

static void tx_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = wdt_register("can_tx", TX_WDT_TIMEOUT_MS);
	struct can_frame f;

	for (;;) {
		wdt_feed(wdt);
		if (k_msgq_get(&tx_q, &f, TX_WAIT) != 0) {
			continue;
		}
		if (can_send(can_dev, &f, TX_WAIT, NULL, NULL) == 0) {
			atomic_inc(&tx_ok);
		} else {
			atomic_inc(&tx_errors);
		}
	}
}

int canlink_init(uint8_t node_id)
{
	const struct can_filter cmd_filter = {
		.id = proto_id(PROTO_FN_CMD, node_id),
		.mask = CAN_STD_ID_MASK,
		.flags = 0,
	};
	int rc;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	rc = can_set_bitrate(can_dev, CAN_BITRATE);
	if (rc == 0) {
		rc = can_set_mode(can_dev, CAN_MODE_NORMAL);
	}
	if (rc == 0) {
		can_init_state_change_callback(&state_cb, state_changed);
		rc = can_add_state_change_callback(can_dev, &state_cb);
	}
	if (rc == 0) {
		rc = can_start(can_dev);
	}
	if (rc != 0) {
		LOG_ERR("CAN start failed (%d)", rc);
		return rc;
	}

	rc = can_add_rx_filter(can_dev, cmd_received, NULL, &cmd_filter);
	if (rc < 0) {
		LOG_ERR("CMD filter failed (%d)", rc);
		return rc;
	}

	const struct can_filter sync_filter = {
		.id = proto_id(PROTO_FN_SYNC, 0U),
		.mask = CAN_STD_ID_MASK,
		.flags = 0,
	};

	rc = can_add_rx_filter(can_dev, sync_received, NULL, &sync_filter);
	if (rc < 0) {
		LOG_ERR("SYNC filter failed (%d)", rc);
		return rc;
	}

	atomic_set(&bus_state, CAN_STATE_ERROR_ACTIVE);
	link_up = true;

	(void)k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_main, NULL,
			      NULL, NULL, TX_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread, "can_tx");

	LOG_INF("CAN up node=%u cmd_id=0x%03x", node_id, cmd_filter.id);
	return 0;
}

int canlink_send(uint16_t id, const uint8_t *data, uint8_t len)
{
	struct can_frame f = {
		.id = id,
		.dlc = can_bytes_to_dlc(len),
		.flags = 0,
	};

	if (!link_up) {
		return -ENETDOWN;
	}
	(void)memcpy(f.data, data, len);
	if (k_msgq_put(&tx_q, &f, K_NO_WAIT) != 0) {
		atomic_inc(&tx_overflows);
		return -ENOBUFS;
	}
	return 0;
}

struct k_msgq *canlink_cmd_queue(void)
{
	return &cmd_q;
}

struct k_msgq *canlink_sync_queue(void)
{
	return &sync_q;
}

void canlink_get_stats(struct canlink_stats *out)
{
	out->tx_ok = (uint32_t)atomic_get(&tx_ok);
	out->tx_errors = (uint32_t)atomic_get(&tx_errors);
	out->tx_overflows = (uint32_t)atomic_get(&tx_overflows);
	out->bus_off_count = (uint32_t)atomic_get(&bus_off_count);
	out->rx_cmds = (uint32_t)atomic_get(&rx_cmds);
	out->rx_dropped = (uint32_t)atomic_get(&rx_dropped);
	out->rx_syncs = (uint32_t)atomic_get(&rx_syncs);
	out->state = (enum can_state)atomic_get(&bus_state);
	out->up = link_up;
}
