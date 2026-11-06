/* SPEC F13, R03: configuration validation, key access and CRC-protected storage form. */
#include <string.h>
#include <zephyr/ztest.h>

#include "cfg.h"

static struct trainer_cfg c;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cfg_defaults(&c);
}

ZTEST_SUITE(cfg, NULL, NULL, before, NULL, NULL);

ZTEST(cfg, test_defaults_are_valid)
{
	zassert_true(cfg_valid(&c));
	zassert_equal(c.node_id, 1);
	zassert_equal(c.capacity, 30);
	zassert_equal(c.auto_rpm, 750);
}

ZTEST(cfg, test_set_and_get_every_key)
{
	static const struct {
		uint8_t key;
		uint32_t value;
	} cases[] = {
		{CFG_KEY_NODE_ID, 42},     {CFG_KEY_CAPACITY, 20},  {CFG_KEY_AUTO_RPM, 900},
		{CFG_KEY_EMITTER_US, 500}, {CFG_KEY_VALVE_MS, 30},  {CFG_KEY_BREAK_PM, 800},
		{CFG_KEY_RESET_PM, 600},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t got;

		zassert_equal(cfg_set_key(&c, cases[i].key, cases[i].value), CFG_OK, "key %u",
			      cases[i].key);
		zassert_equal(cfg_get_key(&c, cases[i].key, &got), CFG_OK);
		zassert_equal(got, cases[i].value, "key %u", cases[i].key);
	}
}

ZTEST(cfg, test_rejects_out_of_range_and_keeps_old_value)
{
	zassert_equal(cfg_set_key(&c, CFG_KEY_AUTO_RPM, 1200), CFG_BAD_VALUE);
	zassert_equal(cfg_set_key(&c, CFG_KEY_NODE_ID, 0), CFG_BAD_VALUE);
	zassert_equal(cfg_set_key(&c, CFG_KEY_NODE_ID, 128), CFG_BAD_VALUE);
	zassert_equal(cfg_set_key(&c, CFG_KEY_CAPACITY, 70000), CFG_BAD_VALUE);
	zassert_equal(c.auto_rpm, 750, "rejected set must not change the config");
	zassert_equal(c.node_id, 1);
}

ZTEST(cfg, test_reset_must_stay_below_break)
{
	zassert_equal(cfg_set_key(&c, CFG_KEY_RESET_PM, 700), CFG_BAD_VALUE, "reset == break");
	zassert_equal(cfg_set_key(&c, CFG_KEY_BREAK_PM, 450), CFG_BAD_VALUE, "break below reset");
	zassert_equal(cfg_set_key(&c, CFG_KEY_RESET_PM, 420), CFG_OK);
	zassert_equal(cfg_set_key(&c, CFG_KEY_BREAK_PM, 450), CFG_OK, "now it fits");
}

ZTEST(cfg, test_unknown_key)
{
	uint32_t v;

	zassert_equal(cfg_get_key(&c, 99, &v), CFG_BAD_KEY);
	zassert_equal(cfg_set_key(&c, 99, 1), CFG_BAD_KEY);
	zassert_equal(cfg_set_key(&c, 99, 100000), CFG_BAD_KEY);
}

ZTEST(cfg, test_serialize_round_trip)
{
	uint8_t blob[CFG_BLOB_LEN];
	struct trainer_cfg back;

	(void)cfg_set_key(&c, CFG_KEY_NODE_ID, 7);
	(void)cfg_set_key(&c, CFG_KEY_AUTO_RPM, 880);
	cfg_serialize(&c, blob);
	zassert_true(cfg_deserialize(blob, sizeof(blob), &back));
	zassert_mem_equal(&back, &c, sizeof(c));
}

ZTEST(cfg, test_corruption_is_detected)
{
	uint8_t blob[CFG_BLOB_LEN];
	struct trainer_cfg back;

	cfg_serialize(&c, blob);
	blob[3] ^= 0x01; /* one bit flip in auto_rpm */
	zassert_false(cfg_deserialize(blob, sizeof(blob), &back), "CRC must catch a bit flip");

	cfg_serialize(&c, blob);
	blob[0] = CFG_VERSION + 1U;
	zassert_false(cfg_deserialize(blob, sizeof(blob), &back), "unknown version");

	cfg_serialize(&c, blob);
	zassert_false(cfg_deserialize(blob, sizeof(blob) - 1U, &back), "wrong length");
}
