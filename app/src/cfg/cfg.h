/*
 * Trainer configuration: defaults, validation, key access and a CRC-protected
 * serialised form for flash. Pure logic: unit-tested on native_sim.
 */
#ifndef TRAINER_CFG_H_
#define TRAINER_CFG_H_

#include <stdbool.h>
#include <stdint.h>

enum cfg_key {
	CFG_KEY_NODE_ID = 1,  /* 1..127, used after the next reboot */
	CFG_KEY_CAPACITY = 2, /* 1..100, used at the next magazine insert */
	CFG_KEY_AUTO_RPM = 3, /* 600..900 */
	CFG_KEY_EMITTER_US = 4, /* 100..20000 */
	CFG_KEY_VALVE_MS = 5, /* 5..40 */
	CFG_KEY_BREAK_PM = 6, /* reset_pm < break_pm <= 1000 */
	CFG_KEY_RESET_PM = 7, /* wall .. break_pm - 1 */
};

/* Result codes match proto_cmd_status so they can go straight into CMD_ACK. */
enum cfg_result {
	CFG_OK = 0,
	CFG_BAD_KEY = 2,
	CFG_BAD_VALUE = 3,
};

#define CFG_VERSION  1U
#define CFG_BLOB_LEN 16U

struct trainer_cfg {
	uint8_t node_id;
	uint8_t capacity;
	uint16_t auto_rpm;
	uint16_t emitter_us;
	uint8_t valve_ms;
	uint16_t break_pm;
	uint16_t reset_pm;
};

void cfg_defaults(struct trainer_cfg *c);
bool cfg_valid(const struct trainer_cfg *c);

enum cfg_result cfg_get_key(const struct trainer_cfg *c, uint8_t key, uint32_t *value);

/* Validates first; on any error the configuration is left unchanged. */
enum cfg_result cfg_set_key(struct trainer_cfg *c, uint8_t key, uint32_t value);

/* Serialised form: version, fields (little-endian), CRC-32 over the rest. */
void cfg_serialize(const struct trainer_cfg *c, uint8_t out[CFG_BLOB_LEN]);

/* False on wrong length, wrong version, bad CRC or invalid values. */
bool cfg_deserialize(const uint8_t *blob, uint32_t len, struct trainer_cfg *c);

#endif /* TRAINER_CFG_H_ */
