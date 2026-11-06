/* IMU sampling thread (LSM6DSO through the Zephyr sensor API) and shot metrics. */
#ifndef TRAINER_IMU_TASK_H_
#define TRAINER_IMU_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#include "imu_metrics.h"

/* Start sampling. Returns -ENODEV if the IMU is not ready (BIT reports it). */
int imu_task_start(void);

/* Metrics for a shot at t_shot_us (k_uptime clock, microseconds). */
bool imu_shot_metrics(uint32_t t_shot_us, struct imu_shot_metrics *out);

/* Latest sample, for the shell. Returns false if none yet. */
bool imu_latest(struct imu_sample *out);

#endif /* TRAINER_IMU_TASK_H_ */
