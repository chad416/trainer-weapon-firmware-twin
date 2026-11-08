#include "bit.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "canlink.h"
#include "cfg_store.h"
#include "control.h"
#include "imu_task.h"
#include "lane.h"
#include "trigger_task.h"
#include "wdt.h"

LOG_MODULE_REGISTER(bit, LOG_LEVEL_INF);

#define REST_MAX_PM 100U /* below the take-up point counts as "at rest" */

static int outputs_rc;
static int adc_rc;
static atomic_t last_mask;

void bit_note_outputs(int init_rc)
{
	outputs_rc = init_rc;
}

void bit_note_adc(int init_rc)
{
	adc_rc = init_rc;
}

uint16_t bit_run(void)
{
	uint16_t mask = 0U;
	struct trigger_snapshot t;
	struct canlink_stats c;

	if (outputs_rc != 0) {
		mask |= BIT_F_OUTPUTS;
	}
	if (adc_rc != 0) {
		mask |= BIT_F_ADC;
	}
	if (!imu_healthy()) {
		mask |= BIT_F_IMU;
	}

	trigger_get_snapshot(&t);
	if ((adc_rc == 0) && (t.samples > 0U) && (t.travel_pm > REST_MAX_PM)) {
		mask |= BIT_F_TRIGGER_NOT_REST;
	}
	if (cfg_store_defaulted()) {
		mask |= BIT_F_CFG_DEFAULTED;
	}
	canlink_get_stats(&c);
	if (!c.up || (c.state != CAN_STATE_ERROR_ACTIVE)) {
		mask |= BIT_F_CAN;
	}
	if (cfg_store_flash_error()) {
		mask |= BIT_F_FLASH;
	}
	if (!wdt_all_watched()) {
		mask |= BIT_F_WATCHDOG;
	}

	bool critical = bit_is_critical(mask);

	atomic_set(&last_mask, mask);
	LOG_INF("BIT mask=0x%04x critical=%s", mask, critical ? "yes" : "no");
	lane_report_bit(mask, critical);
	(void)control_post(critical ? WEV_FAULT_SET : WEV_FAULT_CLEAR);
	return mask;
}

uint16_t bit_last_mask(void)
{
	return (uint16_t)atomic_get(&last_mask);
}
