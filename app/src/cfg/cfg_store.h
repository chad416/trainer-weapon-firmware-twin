/*
 * Persistent configuration (Zephyr settings on the QSPI storage partition),
 * boot counter, and lock-free "live" values for the hot paths.
 */
#ifndef TRAINER_CFG_STORE_H_
#define TRAINER_CFG_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "cfg.h"

/* Load settings, fall back to defaults on corruption, count the boot. */
int cfg_store_init(void);

void cfg_store_get(struct trainer_cfg *out);

/* Validate, save to flash, then apply. Returns a proto_cmd_status value. */
uint8_t cfg_store_set(uint8_t key, uint32_t value);
uint8_t cfg_store_get_key(uint8_t key, uint32_t *value);
uint8_t cfg_store_reset_defaults(void);

/* Node ID fixed for this boot (CAN IDs and filters use it). */
uint8_t cfg_store_node_id(void);
uint32_t cfg_store_boot_count(void);

/* BIT inputs. */
bool cfg_store_defaulted(void);   /* stored config was corrupt -> defaults */
bool cfg_store_flash_error(void); /* settings could not be read or written */

/* Live values, safe to read from any thread or ISR. */
uint32_t cfg_live_emitter_us(void);
uint32_t cfg_live_valve_ms(void);
uint32_t cfg_live_auto_rpm(void);
uint8_t cfg_live_capacity(void);
/* Trigger thresholds plus a generation counter that changes on every update. */
void cfg_live_trigger(uint16_t *break_pm, uint16_t *reset_pm, uint32_t *generation);

/* Debug: store a config blob with a broken CRC (tests R03 after a reboot). */
int cfg_store_corrupt(void);

#endif /* TRAINER_CFG_STORE_H_ */
