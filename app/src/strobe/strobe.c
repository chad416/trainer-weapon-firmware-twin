#include "strobe.h"

static int64_t ceil_div(int64_t a, int64_t b)
{
	return (a >= 0) ? ((a + b - 1) / b) : -((-a) / b);
}

static int64_t mod(int64_t a, int64_t n)
{
	int64_t r = a % n;

	return (r < 0) ? (r + n) : r;
}

bool strobe_cfg_ok(uint8_t n_slots, uint8_t slot)
{
	if (n_slots == 0U) {
		return slot < STROBE_MAX_SLOTS;
	}
	return (n_slots >= 2U) && (n_slots <= STROBE_MAX_SLOTS) && (slot < n_slots);
}

int64_t strobe_pulse_start(int64_t k)
{
	return (k * STROBE_FRAME_US) + STROBE_PULSE_START_US;
}

int64_t strobe_shot_frame(int64_t t_break_us)
{
	return ceil_div(t_break_us + STROBE_MARGIN_US - STROBE_PULSE_START_US, STROBE_FRAME_US);
}

bool strobe_next(const struct strobe_cfg *c, int64_t from_us, int64_t shot_k, struct strobe_pulse *p)
{
	if ((c->n_slots == 0U) || !strobe_cfg_ok(c->n_slots, c->slot)) {
		return false;
	}
	/* First exposure whose pulse starts at or after from_us, then the next one in our slot. */
	int64_t k0 = ceil_div(from_us - STROBE_PULSE_START_US, STROBE_FRAME_US);
	int64_t k_aim = k0 + mod((int64_t)c->slot - k0, c->n_slots);

	p->late = false;
	if (shot_k != STROBE_NO_SHOT) {
		int64_t k_shot = shot_k;

		if (k_shot < k0) {
			k_shot = k0; /* our own exposure is already past: the next one, flagged */
			p->late = true;
		}
		if (k_shot <= k_aim) {
			p->k = k_shot;
			p->start_us = strobe_pulse_start(k_shot);
			p->kind = STROBE_SHOT;
			return true;
		}
	}
	p->k = k_aim;
	p->start_us = strobe_pulse_start(k_aim);
	p->kind = STROBE_AIM;
	return true;
}
