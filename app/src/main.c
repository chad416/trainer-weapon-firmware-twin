/*
 * trainer_fw: firmware for an instrumented training weapon.
 *
 * Start order matters:
 *  1. reset reason + task watchdog
 *  2. outputs forced inactive before anything that can fire runs (SPEC R04)
 *  3. configuration (node ID) before CAN, because CAN IDs depend on it
 *  4. CAN + lane service, weapon control, trigger sampling
 *  5. BIT, then INFO, so the lane learns the node is up and healthy
 */
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "bit.h"
#include "canlink.h"
#include "cfg_store.h"
#include "control.h"
#include "imu_task.h"
#include "lane.h"
#include "outputs.h"
#include "trigger_task.h"
#include "wdt.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	(void)wdt_init();
	LOG_INF("trainer_fw %s reset=%s%s%s", APP_VERSION_STRING,
		wdt_reset_reason_name(wdt_reset_reason()),
		(wdt_last_stalled_thread()[0] != '\0') ? " thread=" : "", wdt_last_stalled_thread());

	int rc = outputs_init();

	bit_note_outputs(rc);
	if (rc != 0) {
		LOG_ERR("outputs init failed (%d)", rc);
	}

	(void)cfg_store_init();
	uint8_t node = cfg_store_node_id();

	if (canlink_init(node) == 0) {
		lane_start(node);
	}

	control_start();

	rc = trigger_task_start();
	bit_note_adc(rc);

	/* A missing IMU is reported by BIT; the weapon then sits in FAULT. */
	(void)imu_task_start();

	/* Let the trigger take a few samples so BIT can see whether it is at rest. */
	k_sleep(K_MSEC(20));
	(void)bit_run();
	lane_send_info();
	LOG_INF("NODE id=%u boot=%u", node, cfg_store_boot_count());
	return 0;
}
