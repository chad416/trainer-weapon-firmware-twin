/*
 * Lane CAN protocol: frame IDs and payload encode/decode.
 * Source of truth for the layout: protocol/trainer.dbc. Little-endian.
 *
 * 11-bit ID = (function << 7) | node_id. Lower function = higher bus priority.
 * CMD / CMD_ACK are ISO-TP (ISO 15765-2) single frames: byte 0 is the PCI
 * (0x0L, L = payload length), so standard diagnostic tools can talk to a node.
 *
 * Pure logic, no Zephyr device access: unit-tested on native_sim.
 */
#ifndef TRAINER_PROTO_H_
#define TRAINER_PROTO_H_

#include <stdbool.h>
#include <stdint.h>

#define PROTO_FN_SYNC        0x1U
#define PROTO_FN_SHOT_EVENT  0x2U
#define PROTO_FN_SHOT_DETAIL 0x3U
#define PROTO_FN_STATUS      0x4U
#define PROTO_FN_EVENT       0x5U
#define PROTO_FN_BIT_RESULT  0x6U
#define PROTO_FN_CMD         0x7U
#define PROTO_FN_CMD_ACK     0x8U
#define PROTO_FN_INFO        0x9U

#define PROTO_NODE_MIN 1U
#define PROTO_NODE_MAX 127U

static inline uint16_t proto_id(uint8_t function, uint8_t node)
{
	return (uint16_t)(((uint16_t)function << 7) | (node & 0x7FU));
}

static inline uint8_t proto_id_function(uint16_t id)
{
	return (uint8_t)((id >> 7) & 0xFU);
}

static inline uint8_t proto_id_node(uint16_t id)
{
	return (uint8_t)(id & 0x7FU);
}

/* ---- payloads ------------------------------------------------------------ */

struct proto_shot_event {
	uint16_t shot_no;
	uint32_t t_us;      /* shot time (lane time once synchronised) */
	uint8_t mode;       /* weapon_selector: 1 SEMI, 2 AUTO */
	bool chambered;     /* a next round is chambered */
	uint8_t rounds_left;
};

struct proto_status {
	uint8_t state; /* weapon_state_id */
	uint8_t sel;   /* weapon_selector */
	bool mag_in;
	bool chambered;
	bool stoppage;
	uint8_t rounds;
	uint16_t faults; /* BIT fault mask */
	uint8_t tx_overflows; /* saturating */
	uint16_t shots;
};

enum proto_event_type {
	PROTO_EV_DRY_FIRE = 1,
	PROTO_EV_MAG_IN = 2,
	PROTO_EV_MAG_OUT = 3,
	PROTO_EV_RACK = 4,
	PROTO_EV_STOPPAGE_START = 5,
	PROTO_EV_STOPPAGE_CLEARED = 6, /* value = clearance time in ms */
	PROTO_EV_SELECTOR = 7,         /* value = new selector */
	PROTO_EV_TAP = 8,
	PROTO_EV_STATE = 9,            /* value = new state */
	PROTO_EV_CLEAR_LATE = 10,      /* value = clearance time in ms, over the window */
};

/* Per-shot detail (SHOT_DETAIL). Values saturate to fit one classic CAN frame. */
struct proto_shot_detail {
	uint16_t shot_no;
	uint8_t takeup_4ms;  /* 4 ms steps, 0..1020 ms */
	uint8_t squeeze_4ms; /* 4 ms steps, 0..1020 ms */
	uint8_t jerk_pm_per_ms;
	uint8_t muzzle_rate_dps;
	int8_t cant_deg;
	int8_t flinch_ddeg; /* 0.1 deg */
};

struct proto_event {
	uint8_t type; /* proto_event_type */
	uint32_t t_us;
	uint16_t value;
};

struct proto_bit_result {
	uint16_t fault_mask;
	bool critical;
};

enum proto_cmd_opcode {
	PROTO_OP_GET = 0x01,
	PROTO_OP_SET = 0x02,        /* applies and saves to flash */
	PROTO_OP_RUN_BIT = 0x03,
	PROTO_OP_INJECT_STOPPAGE = 0x04,
	PROTO_OP_REBOOT = 0x05,
	PROTO_OP_RESET_DEFAULTS = 0x06,
};

enum proto_cmd_status {
	PROTO_ST_OK = 0,
	PROTO_ST_BAD_OPCODE = 1,
	PROTO_ST_BAD_KEY = 2,
	PROTO_ST_BAD_VALUE = 3,
	PROTO_ST_SAVE_FAILED = 4,
	PROTO_ST_READ_ONLY = 5,
};

struct proto_cmd {
	uint8_t opcode;
	uint8_t key;
	uint32_t value;
};

struct proto_cmd_ack {
	uint8_t opcode;
	uint8_t key;
	uint8_t status;
	uint32_t value;
};

struct proto_info {
	uint8_t ver_major;
	uint8_t ver_minor;
	uint8_t ver_patch;
	uint8_t reset_reason;
	uint32_t boot_count;
};

/* Every encoder writes exactly 8 bytes and returns the DLC to send. */
#define PROTO_FRAME_LEN 8U

uint8_t proto_encode_shot_event(const struct proto_shot_event *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_shot_detail(const struct proto_shot_detail *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_status(const struct proto_status *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_event(const struct proto_event *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_bit_result(const struct proto_bit_result *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_cmd_ack(const struct proto_cmd_ack *m, uint8_t out[PROTO_FRAME_LEN]);
uint8_t proto_encode_info(const struct proto_info *m, uint8_t out[PROTO_FRAME_LEN]);

/* Lane side, used by tests and a future lane controller. */
uint8_t proto_encode_cmd(const struct proto_cmd *m, uint8_t out[PROTO_FRAME_LEN]);

/*
 * Decode a CMD frame (ISO-TP single frame, payload 6 bytes).
 * Returns false if the frame is not a valid single frame of that length.
 */
bool proto_decode_cmd(const uint8_t *data, uint8_t len, struct proto_cmd *out);

/* SYNC (lane -> all, ID 0x080): ISO-TP single frame carrying lane_time_us (u32). */
uint8_t proto_encode_sync(uint32_t lane_time_us, uint8_t out[PROTO_FRAME_LEN]);
bool proto_decode_sync(const uint8_t *data, uint8_t len, uint32_t *lane_time_us);

/* Saturating conversions used to build SHOT_DETAIL. */
uint8_t proto_sat_u8(int32_t v);
int8_t proto_sat_i8(int32_t v);

#endif /* TRAINER_PROTO_H_ */
