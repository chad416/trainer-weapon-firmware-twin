#include "proto.h"

#include <string.h>

#define ISOTP_SF_PCI(len) ((uint8_t)((len) & 0x0FU)) /* single frame: high nibble 0 */
#define CMD_PAYLOAD_LEN   6U
#define ACK_PAYLOAD_LEN   7U

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

uint8_t proto_encode_shot_event(const struct proto_shot_event *m, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	put_u16(&out[0], m->shot_no);
	put_u32(&out[2], m->t_us);
	out[6] = (uint8_t)((m->mode & 0x3U) | (m->chambered ? 0x4U : 0U));
	out[7] = m->rounds_left;
	return PROTO_FRAME_LEN;
}

uint8_t proto_sat_u8(int32_t v)
{
	return (v < 0) ? 0U : ((v > (int32_t)UINT8_MAX) ? UINT8_MAX : (uint8_t)v);
}

int8_t proto_sat_i8(int32_t v)
{
	return (v < INT8_MIN) ? INT8_MIN : ((v > INT8_MAX) ? INT8_MAX : (int8_t)v);
}

uint8_t proto_encode_shot_detail(const struct proto_shot_detail *m, uint8_t out[PROTO_FRAME_LEN])
{
	put_u16(&out[0], m->shot_no);
	out[2] = m->takeup_4ms;
	out[3] = m->squeeze_4ms;
	out[4] = m->jerk_pm_per_ms;
	out[5] = m->muzzle_rate_dps;
	out[6] = (uint8_t)m->cant_deg;
	out[7] = (uint8_t)m->flinch_ddeg;
	return PROTO_FRAME_LEN;
}

#define SYNC_PAYLOAD_LEN 4U

uint8_t proto_encode_sync(uint32_t lane_time_us, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	out[0] = ISOTP_SF_PCI(SYNC_PAYLOAD_LEN);
	put_u32(&out[1], lane_time_us);
	return (uint8_t)(1U + SYNC_PAYLOAD_LEN);
}

bool proto_decode_sync(const uint8_t *data, uint8_t len, uint32_t *lane_time_us)
{
	if ((len < (1U + SYNC_PAYLOAD_LEN)) || (data[0] != ISOTP_SF_PCI(SYNC_PAYLOAD_LEN))) {
		return false;
	}
	*lane_time_us = get_u32(&data[1]);
	return true;
}

uint8_t proto_encode_status(const struct proto_status *m, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	out[0] = m->state;
	out[1] = (uint8_t)((m->sel & 0x3U) | (m->mag_in ? 0x4U : 0U) | (m->chambered ? 0x8U : 0U) |
			   (m->stoppage ? 0x10U : 0U));
	out[2] = m->rounds;
	put_u16(&out[3], m->faults);
	out[5] = m->tx_overflows;
	put_u16(&out[6], m->shots);
	return PROTO_FRAME_LEN;
}

uint8_t proto_encode_event(const struct proto_event *m, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	out[0] = m->type;
	put_u32(&out[2], m->t_us);
	put_u16(&out[6], m->value);
	return PROTO_FRAME_LEN;
}

uint8_t proto_encode_bit_result(const struct proto_bit_result *m, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	put_u16(&out[0], m->fault_mask);
	out[2] = m->critical ? 1U : 0U;
	return PROTO_FRAME_LEN;
}

uint8_t proto_encode_cmd_ack(const struct proto_cmd_ack *m, uint8_t out[PROTO_FRAME_LEN])
{
	out[0] = ISOTP_SF_PCI(ACK_PAYLOAD_LEN);
	out[1] = m->opcode;
	out[2] = m->key;
	out[3] = m->status;
	put_u32(&out[4], m->value);
	return PROTO_FRAME_LEN;
}

uint8_t proto_encode_info(const struct proto_info *m, uint8_t out[PROTO_FRAME_LEN])
{
	out[0] = m->ver_major;
	out[1] = m->ver_minor;
	out[2] = m->ver_patch;
	out[3] = m->reset_reason;
	put_u32(&out[4], m->boot_count);
	return PROTO_FRAME_LEN;
}

uint8_t proto_encode_cmd(const struct proto_cmd *m, uint8_t out[PROTO_FRAME_LEN])
{
	(void)memset(out, 0, PROTO_FRAME_LEN);
	out[0] = ISOTP_SF_PCI(CMD_PAYLOAD_LEN);
	out[1] = m->opcode;
	out[2] = m->key;
	put_u32(&out[3], m->value);
	return (uint8_t)(1U + CMD_PAYLOAD_LEN);
}

bool proto_decode_cmd(const uint8_t *data, uint8_t len, struct proto_cmd *out)
{
	/* ISO-TP single frame with exactly the CMD payload; padding after it is allowed. */
	if ((len < (1U + CMD_PAYLOAD_LEN)) || (data[0] != ISOTP_SF_PCI(CMD_PAYLOAD_LEN))) {
		return false;
	}
	out->opcode = data[1];
	out->key = data[2];
	out->value = get_u32(&data[3]);
	return true;
}
