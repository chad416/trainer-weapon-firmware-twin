/*
 * IMU sample history and pre-shot marksmanship metrics. SPEC F10.
 *
 * Weapon frame: x forward along the barrel, y to the left, z up.
 *  - muzzle rate: mean of sqrt(gy^2 + gz^2) over the 300 ms before the shot
 *  - cant:        roll from gravity, atan2(ay, az), at the last sample before the shot
 *  - flinch:      pitch change over the 50 ms before the shot, integral of gy dt;
 *                 positive = muzzle dipping (rotation about +y moves x towards -z)
 *
 * Pure logic, no Zephyr device access: unit-tested on native_sim.
 */
#ifndef TRAINER_IMU_METRICS_H_
#define TRAINER_IMU_METRICS_H_

#include <stdbool.h>
#include <stdint.h>

#define IMU_RING_LEN       128U /* > 500 ms at 208 Hz */
#define IMU_RATE_WINDOW_US 300000U
#define IMU_FLINCH_WINDOW_US 50000U

struct imu_sample {
	uint32_t t_us;
	float gx, gy, gz; /* deg/s */
	float ax, ay, az; /* g */
};

struct imu_ring {
	struct imu_sample s[IMU_RING_LEN];
	uint16_t head; /* next write position */
	uint16_t count;
};

struct imu_shot_metrics {
	float muzzle_rate_dps;
	float cant_deg;
	float flinch_deg;
	uint16_t samples; /* samples inside the 300 ms window */
};

void imu_ring_init(struct imu_ring *r);
void imu_ring_push(struct imu_ring *r, const struct imu_sample *s);

/*
 * Metrics for a shot at t_shot_us (same clock as the samples). Uses only
 * samples at or before the shot. Returns false if there is no sample in the
 * 300 ms window (IMU missing or not yet running); out is zeroed then.
 */
bool imu_compute_metrics(const struct imu_ring *r, uint32_t t_shot_us,
			 struct imu_shot_metrics *out);

#endif /* TRAINER_IMU_METRICS_H_ */
