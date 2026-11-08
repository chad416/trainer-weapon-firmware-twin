#include "cfg.h"

#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "proto.h"
#include "trigger_proc.h"

/* cfg_result values go straight into CMD_ACK.status. */
BUILD_ASSERT((int)CFG_OK == (int)PROTO_ST_OK);
BUILD_ASSERT((int)CFG_BAD_KEY == (int)PROTO_ST_BAD_KEY);
BUILD_ASSERT((int)CFG_BAD_VALUE == (int)PROTO_ST_BAD_VALUE);

/* Build-time defaults; the unit tests build without the app Kconfig. */
#ifndef CONFIG_TRAINER_MAG_CAPACITY
#define CONFIG_TRAINER_MAG_CAPACITY 30
#endif
#ifndef CONFIG_TRAINER_AUTO_RPM
#define CONFIG_TRAINER_AUTO_RPM 750
#endif
#ifndef CONFIG_TRAINER_EMITTER_PULSE_US
#define CONFIG_TRAINER_EMITTER_PULSE_US 2000
#endif
#ifndef CONFIG_TRAINER_VALVE_PULSE_MS
#define CONFIG_TRAINER_VALVE_PULSE_MS 20
#endif

#define WALL_PM TRIG_DEFAULT_WALL_PM /* reset must stay above the take-up wall */

void cfg_defaults(struct trainer_cfg *c)
{
	c->node_id = 1U;
	c->capacity = CONFIG_TRAINER_MAG_CAPACITY;
	c->auto_rpm = CONFIG_TRAINER_AUTO_RPM;
	c->emitter_us = CONFIG_TRAINER_EMITTER_PULSE_US;
	c->valve_ms = CONFIG_TRAINER_VALVE_PULSE_MS;
	c->break_pm = TRIG_DEFAULT_BREAK_PM;
	c->reset_pm = TRIG_DEFAULT_RESET_PM;
}

static bool in_range(uint32_t v, uint32_t lo, uint32_t hi)
{
	return (v >= lo) && (v <= hi);
}

bool cfg_valid(const struct trainer_cfg *c)
{
	return in_range(c->node_id, 1U, 127U) && in_range(c->capacity, 1U, 100U) &&
	       in_range(c->auto_rpm, 600U, 900U) && in_range(c->emitter_us, 100U, 20000U) &&
	       in_range(c->valve_ms, 5U, 40U) && in_range(c->break_pm, WALL_PM + 2U, 1000U) &&
	       in_range(c->reset_pm, WALL_PM + 1U, 999U) && (c->reset_pm < c->break_pm);
}

enum cfg_result cfg_get_key(const struct trainer_cfg *c, uint8_t key, uint32_t *value)
{
	switch (key) {
	case CFG_KEY_NODE_ID:
		*value = c->node_id;
		break;
	case CFG_KEY_CAPACITY:
		*value = c->capacity;
		break;
	case CFG_KEY_AUTO_RPM:
		*value = c->auto_rpm;
		break;
	case CFG_KEY_EMITTER_US:
		*value = c->emitter_us;
		break;
	case CFG_KEY_VALVE_MS:
		*value = c->valve_ms;
		break;
	case CFG_KEY_BREAK_PM:
		*value = c->break_pm;
		break;
	case CFG_KEY_RESET_PM:
		*value = c->reset_pm;
		break;
	default:
		return CFG_BAD_KEY;
	}
	return CFG_OK;
}

enum cfg_result cfg_set_key(struct trainer_cfg *c, uint8_t key, uint32_t value)
{
	struct trainer_cfg next = *c;

	if (value > UINT16_MAX) {
		uint32_t dummy;

		return (cfg_get_key(c, key, &dummy) == CFG_OK) ? CFG_BAD_VALUE : CFG_BAD_KEY;
	}

	switch (key) {
	case CFG_KEY_NODE_ID:
		next.node_id = (uint8_t)MIN(value, UINT8_MAX);
		break;
	case CFG_KEY_CAPACITY:
		next.capacity = (uint8_t)MIN(value, UINT8_MAX);
		break;
	case CFG_KEY_AUTO_RPM:
		next.auto_rpm = (uint16_t)value;
		break;
	case CFG_KEY_EMITTER_US:
		next.emitter_us = (uint16_t)value;
		break;
	case CFG_KEY_VALVE_MS:
		next.valve_ms = (uint8_t)MIN(value, UINT8_MAX);
		break;
	case CFG_KEY_BREAK_PM:
		next.break_pm = (uint16_t)value;
		break;
	case CFG_KEY_RESET_PM:
		next.reset_pm = (uint16_t)value;
		break;
	default:
		return CFG_BAD_KEY;
	}

	if (!cfg_valid(&next)) {
		return CFG_BAD_VALUE;
	}
	*c = next;
	return CFG_OK;
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

#define CRC_OFFSET 12U

void cfg_serialize(const struct trainer_cfg *c, uint8_t out[CFG_BLOB_LEN])
{
	out[0] = CFG_VERSION;
	out[1] = c->node_id;
	out[2] = c->capacity;
	put_u16(&out[3], c->auto_rpm);
	put_u16(&out[5], c->emitter_us);
	out[7] = c->valve_ms;
	put_u16(&out[8], c->break_pm);
	put_u16(&out[10], c->reset_pm);

	uint32_t crc = crc32_ieee(out, CRC_OFFSET);

	out[12] = (uint8_t)(crc & 0xFFU);
	out[13] = (uint8_t)((crc >> 8) & 0xFFU);
	out[14] = (uint8_t)((crc >> 16) & 0xFFU);
	out[15] = (uint8_t)(crc >> 24);
}

bool cfg_deserialize(const uint8_t *blob, uint32_t len, struct trainer_cfg *c)
{
	struct trainer_cfg tmp;

	if ((len != CFG_BLOB_LEN) || (blob[0] != CFG_VERSION)) {
		return false;
	}

	uint32_t stored = (uint32_t)blob[12] | ((uint32_t)blob[13] << 8) |
			  ((uint32_t)blob[14] << 16) | ((uint32_t)blob[15] << 24);

	if (stored != crc32_ieee(blob, CRC_OFFSET)) {
		return false;
	}

	tmp.node_id = blob[1];
	tmp.capacity = blob[2];
	tmp.auto_rpm = get_u16(&blob[3]);
	tmp.emitter_us = get_u16(&blob[5]);
	tmp.valve_ms = blob[7];
	tmp.break_pm = get_u16(&blob[8]);
	tmp.reset_pm = get_u16(&blob[10]);

	if (!cfg_valid(&tmp)) {
		return false;
	}
	*c = tmp;
	return true;
}
