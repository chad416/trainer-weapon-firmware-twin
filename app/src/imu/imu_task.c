#include "imu_task.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "wdt.h"

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_STACK_SIZE     1536
#define IMU_PRIO           5
#define IMU_PERIOD_US      4808U /* 208 Hz, matching the sensor ODR in the overlay */
#define IMU_WDT_TIMEOUT_MS 500U
#define STANDARD_GRAVITY   9.80665f
#define RAD_TO_DEG         57.29577951f
#define IMU_HEALTH_MS      50U /* about 10 sample periods */

static const struct device *const imu = DEVICE_DT_GET_OR_NULL(DT_ALIAS(accel0));

K_SEM_DEFINE(imu_sem, 0, 1);
K_THREAD_STACK_DEFINE(imu_stack, IMU_STACK_SIZE);

static struct k_thread imu_thread;
static struct k_spinlock ring_lock;
static struct imu_ring ring;
static atomic_t good_samples;
static atomic_t last_good_ms;

static void imu_tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&imu_sem);
}

K_TIMER_DEFINE(imu_timer, imu_tick, NULL);

static bool read_sample(struct imu_sample *s)
{
	struct sensor_value acc[3];
	struct sensor_value gyr[3];

	if ((sensor_sample_fetch(imu) != 0) ||
	    (sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, acc) != 0) ||
	    (sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyr) != 0)) {
		return false;
	}

	s->ax = (float)sensor_value_to_double(&acc[0]) / STANDARD_GRAVITY;
	s->ay = (float)sensor_value_to_double(&acc[1]) / STANDARD_GRAVITY;
	s->az = (float)sensor_value_to_double(&acc[2]) / STANDARD_GRAVITY;
	s->gx = (float)sensor_value_to_double(&gyr[0]) * RAD_TO_DEG;
	s->gy = (float)sensor_value_to_double(&gyr[1]) * RAD_TO_DEG;
	s->gz = (float)sensor_value_to_double(&gyr[2]) * RAD_TO_DEG;
	return true;
}

static void imu_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = wdt_register("imu", IMU_WDT_TIMEOUT_MS);
	struct imu_sample s;
	uint32_t errors = 0U;

	for (;;) {
		(void)k_sem_take(&imu_sem, K_FOREVER);
		wdt_feed(wdt);

		s.t_us = (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
		if (!read_sample(&s)) {
			if ((errors++ % 1000U) == 0U) {
				LOG_WRN("IMU read failed (%u)", errors);
			}
			continue;
		}

		k_spinlock_key_t key = k_spin_lock(&ring_lock);

		imu_ring_push(&ring, &s);
		k_spin_unlock(&ring_lock, key);
		atomic_set(&last_good_ms, (atomic_val_t)k_uptime_get_32());
		atomic_inc(&good_samples);
	}
}

bool imu_healthy(void)
{
	if (atomic_get(&good_samples) == 0) {
		return false; /* not started (IMU missing) or no good read yet */
	}
	uint32_t age_ms = k_uptime_get_32() - (uint32_t)atomic_get(&last_good_ms);

	return age_ms <= IMU_HEALTH_MS;
}

int imu_task_start(void)
{
	if ((imu == NULL) || !device_is_ready(imu)) {
		LOG_ERR("IMU not ready");
		return -ENODEV;
	}

	imu_ring_init(&ring);
	(void)k_thread_create(&imu_thread, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack), imu_main,
			      NULL, NULL, NULL, IMU_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&imu_thread, "imu");
	k_timer_start(&imu_timer, K_USEC(IMU_PERIOD_US), K_USEC(IMU_PERIOD_US));
	return 0;
}

bool imu_shot_metrics(uint32_t t_shot_us, struct imu_shot_metrics *out)
{
	/* About 60 samples of float maths: a few microseconds under the lock. */
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	bool ok = imu_compute_metrics(&ring, t_shot_us, out);

	k_spin_unlock(&ring_lock, key);
	return ok;
}

bool imu_latest(struct imu_sample *out)
{
	bool ok = false;
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	if (ring.count > 0U) {
		*out = ring.s[(ring.head + IMU_RING_LEN - 1U) % IMU_RING_LEN];
		ok = true;
	}
	k_spin_unlock(&ring_lock, key);
	return ok;
}
