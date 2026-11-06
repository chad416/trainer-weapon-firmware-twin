#include "imu_metrics.h"

#include <math.h>
#include <string.h>

#define RAD_TO_DEG 57.29577951f

void imu_ring_init(struct imu_ring *r)
{
	(void)memset(r, 0, sizeof(*r));
}

void imu_ring_push(struct imu_ring *r, const struct imu_sample *s)
{
	r->s[r->head] = *s;
	r->head = (uint16_t)((r->head + 1U) % IMU_RING_LEN);
	if (r->count < IMU_RING_LEN) {
		r->count++;
	}
}

/* i = 0 is the newest sample. */
static const struct imu_sample *nth_newest(const struct imu_ring *r, uint16_t i)
{
	uint16_t idx = (uint16_t)((r->head + IMU_RING_LEN - 1U - i) % IMU_RING_LEN);

	return &r->s[idx];
}

/* Age of a sample relative to the shot; negative if it is after the shot. Wrap-safe. */
static int32_t age_us(uint32_t t_shot_us, uint32_t t_us)
{
	return (int32_t)(t_shot_us - t_us);
}

bool imu_compute_metrics(const struct imu_ring *r, uint32_t t_shot_us,
			 struct imu_shot_metrics *out)
{
	float rate_sum = 0.0f;
	float flinch = 0.0f;
	uint16_t n = 0U;
	const struct imu_sample *last = NULL;
	const struct imu_sample *newer = NULL; /* the sample after the current one, in time */

	(void)memset(out, 0, sizeof(*out));

	for (uint16_t i = 0U; i < r->count; i++) {
		const struct imu_sample *s = nth_newest(r, i);
		int32_t age = age_us(t_shot_us, s->t_us);

		if (age < 0) {
			continue; /* after the shot */
		}
		if (age > (int32_t)IMU_RATE_WINDOW_US) {
			break; /* older samples are outside every window */
		}
		if (last == NULL) {
			last = s;
		}

		rate_sum += sqrtf((s->gy * s->gy) + (s->gz * s->gz));
		n++;

		/* Rectangle rule: each sample's rate holds until the next sample (or the shot). */
		if (age <= (int32_t)IMU_FLINCH_WINDOW_US) {
			uint32_t until = (newer != NULL) ? newer->t_us : t_shot_us;
			float dt_s = (float)(until - s->t_us) * 1e-6f;

			flinch += s->gy * dt_s;
		}
		newer = s;
	}

	if (n == 0U) {
		return false;
	}

	out->muzzle_rate_dps = rate_sum / (float)n;
	out->cant_deg = atan2f(last->ay, last->az) * RAD_TO_DEG;
	out->flinch_deg = flinch;
	out->samples = n;
	return true;
}
