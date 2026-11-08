#include "cfg_store.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "proto.h"

LOG_MODULE_REGISTER(cfg, LOG_LEVEL_INF);

#define KEY_CFG   "trainer/cfg"
#define KEY_BOOTS "trainer/boots"

K_MUTEX_DEFINE(cfg_lock);

static struct trainer_cfg current;
static uint8_t boot_node = 1U;
static uint32_t boot_count;
static bool found_cfg;
static bool blob_bad;
static bool defaulted;
static bool flash_error;

static atomic_t live_emitter_us;
static atomic_t live_valve_ms;
static atomic_t live_rpm;
static atomic_t live_capacity;
static atomic_t live_trigger; /* break_pm << 16 | reset_pm */
static atomic_t live_generation;

/* ---- settings handler -------------------------------------------------------- */

static int h_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(key, "cfg", &next) && (next == NULL)) {
		uint8_t blob[CFG_BLOB_LEN];
		struct trainer_cfg tmp;

		found_cfg = true;
		if (len != sizeof(blob)) {
			blob_bad = true;
			return 0;
		}
		if (read_cb(cb_arg, blob, sizeof(blob)) != (ssize_t)sizeof(blob)) {
			flash_error = true;
			return 0;
		}
		if (cfg_deserialize(blob, len, &tmp)) {
			current = tmp;
		} else {
			blob_bad = true;
		}
		return 0;
	}

	if (settings_name_steq(key, "boots", &next) && (next == NULL)) {
		if ((len == sizeof(boot_count)) &&
		    (read_cb(cb_arg, &boot_count, sizeof(boot_count)) == (ssize_t)sizeof(boot_count))) {
			return 0;
		}
		boot_count = 0U;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(trainer, "trainer", NULL, h_set, NULL, NULL);

/* ---- helpers ----------------------------------------------------------------- */

/*
 * Default node ID from the MCU unique ID, so two fresh units on one bus are
 * unlikely to collide. Field units get an explicit ID through SET NODE_ID.
 */
static uint8_t node_from_uid(void)
{
	uint8_t id[16];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));
	uint32_t sum = 0U;

	if (n <= 0) {
		return 1U;
	}
	for (ssize_t i = 0; i < n; i++) {
		sum += id[i];
	}
	return (uint8_t)(1U + (sum % PROTO_NODE_MAX));
}

static void factory_defaults(struct trainer_cfg *c)
{
	cfg_defaults(c);
	c->node_id = node_from_uid();
}

static void publish(const struct trainer_cfg *c)
{
	atomic_set(&live_emitter_us, c->emitter_us);
	atomic_set(&live_valve_ms, c->valve_ms);
	atomic_set(&live_rpm, c->auto_rpm);
	atomic_set(&live_capacity, c->capacity);
	atomic_set(&live_trigger, (atomic_val_t)(((uint32_t)c->break_pm << 16) | c->reset_pm));
	atomic_inc(&live_generation);
}

static int save_cfg(const struct trainer_cfg *c)
{
	uint8_t blob[CFG_BLOB_LEN];

	cfg_serialize(c, blob);
	return settings_save_one(KEY_CFG, blob, sizeof(blob));
}

/* ---- public ------------------------------------------------------------------ */

int cfg_store_init(void)
{
	factory_defaults(&current);

	int rc = settings_subsys_init();

	if (rc == 0) {
		rc = settings_load_subtree("trainer");
	}
	if (rc != 0) {
		LOG_ERR("settings unavailable (%d), using defaults", rc);
		flash_error = true;
		factory_defaults(&current);
		boot_node = current.node_id;
		publish(&current);
		return rc;
	}

	if (blob_bad) {
		LOG_WRN("stored config corrupt, defaults restored");
		defaulted = true;
		factory_defaults(&current);
		if (save_cfg(&current) != 0) {
			flash_error = true;
		}
	} else if (!found_cfg) {
		LOG_INF("no stored config, using defaults");
	}

	boot_count++;
	if (settings_save_one(KEY_BOOTS, &boot_count, sizeof(boot_count)) != 0) {
		flash_error = true;
	}

	boot_node = current.node_id;
	publish(&current);
	LOG_INF("config node=%u capacity=%u rpm=%u boot=%u", current.node_id, current.capacity,
		current.auto_rpm, boot_count);
	return 0;
}

void cfg_store_get(struct trainer_cfg *out)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	*out = current;
	k_mutex_unlock(&cfg_lock);
}

uint8_t cfg_store_get_key(uint8_t key, uint32_t *value)
{
	struct trainer_cfg c;

	cfg_store_get(&c);
	return (uint8_t)cfg_get_key(&c, key, value);
}

uint8_t cfg_store_set(uint8_t key, uint32_t value)
{
	uint8_t status;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	struct trainer_cfg next = current;
	enum cfg_result r = cfg_set_key(&next, key, value);

	if (r != CFG_OK) {
		status = (uint8_t)r;
	} else if (save_cfg(&next) != 0) {
		flash_error = true;
		status = PROTO_ST_SAVE_FAILED; /* not applied: RAM and flash stay consistent */
	} else {
		current = next;
		publish(&current);
		status = PROTO_ST_OK;
	}
	k_mutex_unlock(&cfg_lock);

	if (status == PROTO_ST_OK) {
		LOG_INF("CFG key=%u value=%u saved", key, value);
	}
	return status;
}

uint8_t cfg_store_reset_defaults(void)
{
	uint8_t status = PROTO_ST_OK;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	struct trainer_cfg next;

	factory_defaults(&next);
	if (save_cfg(&next) != 0) {
		flash_error = true;
		status = PROTO_ST_SAVE_FAILED;
	} else {
		current = next;
		publish(&current);
	}
	k_mutex_unlock(&cfg_lock);
	return status;
}

int cfg_store_corrupt(void)
{
	uint8_t blob[CFG_BLOB_LEN];
	struct trainer_cfg c;

	cfg_store_get(&c);
	cfg_serialize(&c, blob);
	blob[CFG_BLOB_LEN - 1U] ^= 0xFFU; /* break the CRC */
	return settings_save_one(KEY_CFG, blob, sizeof(blob));
}

uint8_t cfg_store_node_id(void)
{
	return boot_node;
}

uint32_t cfg_store_boot_count(void)
{
	return boot_count;
}

bool cfg_store_defaulted(void)
{
	return defaulted;
}

bool cfg_store_flash_error(void)
{
	return flash_error;
}

uint32_t cfg_live_emitter_us(void)
{
	return (uint32_t)atomic_get(&live_emitter_us);
}

uint32_t cfg_live_valve_ms(void)
{
	return (uint32_t)atomic_get(&live_valve_ms);
}

uint32_t cfg_live_auto_rpm(void)
{
	return (uint32_t)atomic_get(&live_rpm);
}

uint8_t cfg_live_capacity(void)
{
	return (uint8_t)atomic_get(&live_capacity);
}

void cfg_live_trigger(uint16_t *break_pm, uint16_t *reset_pm, uint32_t *generation)
{
	uint32_t packed = (uint32_t)atomic_get(&live_trigger);

	*break_pm = (uint16_t)(packed >> 16);
	*reset_pm = (uint16_t)(packed & 0xFFFFU);
	*generation = (uint32_t)atomic_get(&live_generation);
}

uint32_t cfg_live_trigger_generation(void)
{
	return (uint32_t)atomic_get(&live_generation);
}
