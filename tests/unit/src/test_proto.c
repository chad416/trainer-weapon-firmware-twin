/* SPEC §6: firmware encoders must match the DBC byte for byte (vectors generated from the DBC). */
#include <zephyr/ztest.h>

#include "proto.h"
#include "proto_vectors.h"

ZTEST_SUITE(proto, NULL, NULL, NULL, NULL, NULL);

ZTEST(proto, test_ids)
{
	zassert_equal(proto_id(PROTO_FN_SYNC, 0), 0x080);
	zassert_equal(proto_id(PROTO_FN_SHOT_EVENT, 1), 0x101);
	zassert_equal(proto_id(PROTO_FN_CMD, 2), 0x382);
	zassert_equal(proto_id(PROTO_FN_INFO, 127), 0x4FF);
	zassert_equal(proto_id_function(0x382), PROTO_FN_CMD);
	zassert_equal(proto_id_node(0x382), 2);
}

ZTEST(proto, test_shot_event_matches_dbc)
{
	struct proto_shot_event m = {
		.shot_no = 4660, .t_us = 2712847316U, .mode = 2, .chambered = true, .rounds_left = 29};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_shot_event(&m, out), sizeof(VEC_SHOT_EVENT));
	zassert_mem_equal(out, VEC_SHOT_EVENT, sizeof(VEC_SHOT_EVENT));
}

ZTEST(proto, test_sync_matches_dbc_and_round_trips)
{
	uint8_t out[PROTO_FRAME_LEN];
	uint32_t back;
	uint8_t len = proto_encode_sync(3735928559U, out);

	zassert_equal(len, sizeof(VEC_SYNC));
	zassert_mem_equal(out, VEC_SYNC, sizeof(VEC_SYNC));
	zassert_true(proto_decode_sync(out, len, &back));
	zassert_equal(back, 3735928559U);
	zassert_false(proto_decode_sync(VEC_CMD, sizeof(VEC_CMD), &back), "a CMD is not a SYNC");
}

ZTEST(proto, test_shot_detail_matches_dbc)
{
	/* take-up 120 ms and squeeze 44 ms in 4 ms steps */
	struct proto_shot_detail m = {.shot_no = 7, .takeup_4ms = 30, .squeeze_4ms = 11,
				      .jerk_pm_per_ms = 30, .muzzle_rate_dps = 12, .cant_deg = -5,
				      .flinch_ddeg = 15};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_shot_detail(&m, out), sizeof(VEC_SHOT_DETAIL));
	zassert_mem_equal(out, VEC_SHOT_DETAIL, sizeof(VEC_SHOT_DETAIL));
}

ZTEST(proto, test_saturation)
{
	zassert_equal(proto_sat_u8(-3), 0);
	zassert_equal(proto_sat_u8(300), 255);
	zassert_equal(proto_sat_i8(-200), -128);
	zassert_equal(proto_sat_i8(200), 127);
	zassert_equal(proto_sat_i8(-5), -5);
}

ZTEST(proto, test_status_matches_dbc)
{
	struct proto_status m = {.state = 5, .sel = 2, .mag_in = true, .chambered = true,
				 .stoppage = false, .rounds = 28, .faults = 264, .tx_overflows = 3,
				 .tx_errors = 5, .shots = 515};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_status(&m, out), sizeof(VEC_STATUS));
	zassert_mem_equal(out, VEC_STATUS, sizeof(VEC_STATUS));

	/* tx_errors has 3 bits: it saturates at 7 and leaves the other flags alone. */
	m.tx_errors = 200;
	(void)proto_encode_status(&m, out);
	zassert_equal(out[1] >> 5, 7);
	zassert_equal(out[1] & 0x1FU, VEC_STATUS[1] & 0x1FU);
}

ZTEST(proto, test_event_matches_dbc)
{
	struct proto_event m = {.type = 6, .t_us = 16909060U, .value = 2500};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_event(&m, out), sizeof(VEC_EVENT));
	zassert_mem_equal(out, VEC_EVENT, sizeof(VEC_EVENT));
}

ZTEST(proto, test_bit_result_matches_dbc)
{
	struct proto_bit_result m = {.fault_mask = 20, .critical = true};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_bit_result(&m, out), sizeof(VEC_BIT_RESULT));
	zassert_mem_equal(out, VEC_BIT_RESULT, sizeof(VEC_BIT_RESULT));
}

ZTEST(proto, test_cmd_matches_dbc_and_round_trips)
{
	struct proto_cmd m = {.opcode = 2, .key = 3, .value = 900};
	struct proto_cmd back;
	uint8_t out[PROTO_FRAME_LEN];
	uint8_t len = proto_encode_cmd(&m, out);

	zassert_equal(len, sizeof(VEC_CMD));
	zassert_mem_equal(out, VEC_CMD, sizeof(VEC_CMD));
	zassert_true(proto_decode_cmd(out, len, &back));
	zassert_equal(back.opcode, 2);
	zassert_equal(back.key, 3);
	zassert_equal(back.value, 900);
}

ZTEST(proto, test_cmd_ack_matches_dbc)
{
	struct proto_cmd_ack m = {.opcode = 2, .key = 3, .status = 0, .value = 900};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_cmd_ack(&m, out), sizeof(VEC_CMD_ACK));
	zassert_mem_equal(out, VEC_CMD_ACK, sizeof(VEC_CMD_ACK));
}

ZTEST(proto, test_info_matches_dbc)
{
	struct proto_info m = {.ver_major = 0, .ver_minor = 2, .ver_patch = 0, .reset_reason = 3,
			       .boot_count = 42};
	uint8_t out[PROTO_FRAME_LEN];

	zassert_equal(proto_encode_info(&m, out), sizeof(VEC_INFO));
	zassert_mem_equal(out, VEC_INFO, sizeof(VEC_INFO));
}

ZTEST(proto, test_cmd_decode_rejects_bad_frames)
{
	struct proto_cmd c;
	uint8_t padded[8] = {0x06, 0x01, 0x02, 0, 0, 0, 0, 0xCC};
	uint8_t first_frame[8] = {0x10, 0x08, 0x01, 0x02, 0, 0, 0, 0}; /* ISO-TP first frame */
	uint8_t wrong_len[7] = {0x05, 0x01, 0x02, 0, 0, 0, 0};

	zassert_true(proto_decode_cmd(padded, 8, &c), "padding after the payload is allowed");
	zassert_false(proto_decode_cmd(first_frame, 8, &c), "multi-frame is not a CMD");
	zassert_false(proto_decode_cmd(wrong_len, 7, &c), "PCI length must be 6");
	zassert_false(proto_decode_cmd(padded, 6, &c), "frame shorter than the payload");
}
