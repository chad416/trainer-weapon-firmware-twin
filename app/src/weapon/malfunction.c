#include "malfunction.h"

#include <string.h>

#define ST_OK        0U
#define ST_BAD_KEY   2U
#define ST_BAD_VALUE 3U

void malf_init(struct malf *m)
{
	(void)memset(m, 0, sizeof(*m));
	m->mode = MALF_OFF;
	m->round = 5U;
}

uint8_t malf_set_key(struct malf *m, uint8_t key, uint32_t value)
{
	switch (key) {
	case MALF_KEY_MODE:
		if (value > MALF_RANDOM) {
			return ST_BAD_VALUE;
		}
		m->mode = (uint8_t)value;
		m->done_this_mag = false;
		return ST_OK;
	case MALF_KEY_ROUND:
		if ((value < 1U) || (value > 100U)) {
			return ST_BAD_VALUE;
		}
		m->round = (uint8_t)value;
		return ST_OK;
	case MALF_KEY_PROB_PM:
		if (value > 1000U) {
			return ST_BAD_VALUE;
		}
		m->prob_pm = (uint16_t)value;
		return ST_OK;
	case MALF_KEY_CLEAR_WINDOW_MS:
		if ((value != 0U) && ((value < 100U) || (value > 60000U))) {
			return ST_BAD_VALUE;
		}
		m->clear_window_ms = (uint16_t)value;
		return ST_OK;
	default:
		return ST_BAD_KEY;
	}
}

uint8_t malf_get_key(const struct malf *m, uint8_t key, uint32_t *value)
{
	switch (key) {
	case MALF_KEY_MODE:
		*value = m->mode;
		return ST_OK;
	case MALF_KEY_ROUND:
		*value = m->round;
		return ST_OK;
	case MALF_KEY_PROB_PM:
		*value = m->prob_pm;
		return ST_OK;
	case MALF_KEY_CLEAR_WINDOW_MS:
		*value = m->clear_window_ms;
		return ST_OK;
	default:
		return ST_BAD_KEY;
	}
}

void malf_on_mag_insert(struct malf *m)
{
	m->rounds_from_mag = 0U;
	m->done_this_mag = false;
}

bool malf_on_round(struct malf *m, uint32_t random)
{
	if (m->rounds_from_mag < UINT16_MAX) {
		m->rounds_from_mag++;
	}

	switch (m->mode) {
	case MALF_AT_ROUND:
		if (!m->done_this_mag && (m->rounds_from_mag == m->round)) {
			m->done_this_mag = true;
			return true;
		}
		return false;
	case MALF_RANDOM:
		return (random % 1000U) < m->prob_pm;
	default:
		return false;
	}
}

bool malf_clear_late(const struct malf *m, uint32_t clear_ms)
{
	return (m->clear_window_ms != 0U) && (clear_ms > m->clear_window_ms);
}
