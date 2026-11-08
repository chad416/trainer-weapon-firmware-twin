/* CAN transport: TX queue + thread, CMD receive filter, bus state tracking. */
#ifndef TRAINER_CANLINK_H_
#define TRAINER_CANLINK_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/can.h>

struct canlink_stats {
	uint32_t tx_ok;
	uint32_t tx_errors;    /* frames dropped after TX_ATTEMPTS failed sends */
	uint32_t tx_retries;   /* failed sends that were retried */
	uint32_t tx_overflows; /* frames dropped because the TX queue was full */
	uint32_t bus_off_count;
	uint32_t rx_cmds;    /* CMD frames delivered by the controller */
	uint32_t rx_dropped; /* CMD frames dropped because the queue was full */
	uint32_t rx_syncs;
	enum can_state state;
	bool up;
};

/* Configure FDCAN1 (classic CAN, 500 kbit/s), start it and the TX thread. */
int canlink_init(uint8_t node_id);

/*
 * Queue a frame (len <= 8). Never blocks. Returns 0, -ENOBUFS (counted) if
 * the queue is full, or -EINVAL if len is too long.
 */
int canlink_send(uint16_t id, const uint8_t *data, uint8_t len);

/* A received SYNC: lane time and the local time it arrived (captured in the RX ISR). */
struct canlink_sync {
	uint64_t local_us;
	uint32_t lane_us;
};

/* Queues for the lane thread (k_poll on both, then drain with K_NO_WAIT). */
struct k_msgq *canlink_cmd_queue(void);  /* struct can_frame, CMD to this node */
struct k_msgq *canlink_sync_queue(void); /* struct canlink_sync */

void canlink_get_stats(struct canlink_stats *out);

#endif /* TRAINER_CANLINK_H_ */
