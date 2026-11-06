/* SPEC F10: IMU pre-shot metrics. */
#include <math.h>
#include <zephyr/ztest.h>

#include "imu_metrics.h"

#define PERIOD_US 5000U /* 200 Hz */

static struct imu_ring ring;

static void push(uint32_t t_us, float gy, float gz, float ay, float az)
{
	struct imu_sample s = {.t_us = t_us, .gx = 0.0f, .gy = gy, .gz = gz,
			       .ax = 0.0f, .ay = ay, .az = az};

	imu_ring_push(&ring, &s);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	imu_ring_init(&ring);
}

ZTEST_SUITE(imu, NULL, NULL, before, NULL, NULL);

ZTEST(imu, test_no_samples)
{
	struct imu_shot_metrics m;

	zassert_false(imu_compute_metrics(&ring, 1000000U, &m));
	zassert_equal(m.samples, 0);
}

ZTEST(imu, test_constant_rate_and_flinch)
{
	struct imu_shot_metrics m;

	/* 1 s of a steady 20 deg/s pitch rate; shot exactly on a sample. */
	for (uint32_t t = 0U; t <= 1000000U; t += PERIOD_US) {
		push(t, 20.0f, 0.0f, 0.0f, 1.0f);
	}
	zassert_true(imu_compute_metrics(&ring, 1000000U, &m));
	zassert_within(m.muzzle_rate_dps, 20.0f, 0.01f);
	/* 50 ms at 20 deg/s = 1 deg: samples at -50..-5 ms cover 50 ms exactly. */
	zassert_within(m.flinch_deg, 1.0f, 0.11f);
	zassert_within(m.samples, 61, 1, "300 ms at 200 Hz");
}

ZTEST(imu, test_rate_uses_pitch_and_yaw_not_roll)
{
	struct imu_shot_metrics m;

	for (uint32_t t = 0U; t <= 400000U; t += PERIOD_US) {
		struct imu_sample s = {.t_us = t, .gx = 100.0f, .gy = 3.0f, .gz = 4.0f,
				       .ay = 0.0f, .az = 1.0f};

		imu_ring_push(&ring, &s);
	}
	zassert_true(imu_compute_metrics(&ring, 400000U, &m));
	zassert_within(m.muzzle_rate_dps, 5.0f, 0.01f, "sqrt(3^2 + 4^2), roll ignored");
}

ZTEST(imu, test_cant_from_gravity)
{
	struct imu_shot_metrics m;
	float r = 10.0f * 3.14159265f / 180.0f;

	for (uint32_t t = 0U; t <= 400000U; t += PERIOD_US) {
		push(t, 0.0f, 0.0f, sinf(r), cosf(r));
	}
	zassert_true(imu_compute_metrics(&ring, 400000U, &m));
	zassert_within(m.cant_deg, 10.0f, 0.01f);
}

ZTEST(imu, test_flinch_only_counts_last_50_ms)
{
	struct imu_shot_metrics m;

	/* Steady until 950 ms, then a dip of 30 deg/s. Shot at 1000 ms. */
	for (uint32_t t = 0U; t <= 1000000U; t += PERIOD_US) {
		push(t, (t >= 950000U) ? 30.0f : 0.0f, 0.0f, 0.0f, 1.0f);
	}
	zassert_true(imu_compute_metrics(&ring, 1000000U, &m));
	zassert_within(m.flinch_deg, 1.5f, 0.16f, "30 deg/s for 50 ms");

	/* A dip long before the shot does not count as flinch. */
	imu_ring_init(&ring);
	for (uint32_t t = 0U; t <= 1000000U; t += PERIOD_US) {
		push(t, ((t >= 700000U) && (t < 800000U)) ? 30.0f : 0.0f, 0.0f, 0.0f, 1.0f);
	}
	zassert_true(imu_compute_metrics(&ring, 1000000U, &m));
	zassert_within(m.flinch_deg, 0.0f, 0.001f);
	zassert_true(m.muzzle_rate_dps > 5.0f, "but it does raise the 300 ms rate");
}

ZTEST(imu, test_samples_after_the_shot_are_ignored)
{
	struct imu_shot_metrics m;

	for (uint32_t t = 0U; t <= 500000U; t += PERIOD_US) {
		push(t, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	for (uint32_t t = 505000U; t <= 600000U; t += PERIOD_US) {
		push(t, 500.0f, 500.0f, 0.0f, 1.0f); /* recoil after the shot */
	}
	zassert_true(imu_compute_metrics(&ring, 500000U, &m));
	zassert_within(m.muzzle_rate_dps, 0.0f, 0.001f);
	zassert_within(m.flinch_deg, 0.0f, 0.001f);
}

ZTEST(imu, test_clock_wraparound)
{
	struct imu_shot_metrics m;
	uint32_t t0 = UINT32_MAX - 200000U;

	for (uint32_t i = 0U; i <= 80U; i++) {
		push(t0 + (i * PERIOD_US), 20.0f, 0.0f, 0.0f, 1.0f);
	}
	zassert_true(imu_compute_metrics(&ring, t0 + (80U * PERIOD_US), &m));
	zassert_within(m.muzzle_rate_dps, 20.0f, 0.01f);
	zassert_within(m.flinch_deg, 1.0f, 0.11f);
}
