#include "trigger_task.h"

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "cfg_store.h"
#include "control.h"
#include "local_clock.h"
#include "wdt.h"

LOG_MODULE_REGISTER(trigger, LOG_LEVEL_INF);

#define TRIGGER_STACK_SIZE     1024
#define TRIGGER_PRIO           2 /* above control: a break must not wait behind other work */
#define TRIGGER_WDT_TIMEOUT_MS 500U
#define TRIGGER_WDT_FEED_EVERY 50U /* samples */

#if !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "The board must define zephyr,user io-channels for the trigger potentiometer"
#endif

static const struct adc_dt_spec trigger_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

K_SEM_DEFINE(sample_sem, 0, 1);
K_THREAD_STACK_DEFINE(trigger_stack, TRIGGER_STACK_SIZE);

static struct k_thread trigger_thread;
static struct trig_proc proc;
static struct k_spinlock snap_lock;
static struct trigger_snapshot snap;
static atomic_t stall_requested;

static void sample_tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&sample_sem);
}

K_TIMER_DEFINE(sample_timer, sample_tick, NULL);

void trigger_get_snapshot(struct trigger_snapshot *out)
{
	k_spinlock_key_t key = k_spin_lock(&snap_lock);

	*out = snap;
	k_spin_unlock(&snap_lock, key);
}

void trigger_debug_stall(void)
{
	atomic_set(&stall_requested, 1);
}

/* Calibration from the live configuration. Re-initialising disarms the trigger
 * until it is released, so a threshold change can never fire a shot.
 */
static void load_calibration(uint32_t *generation)
{
	struct trig_cal cal;
	uint16_t break_pm;
	uint16_t reset_pm;

	cfg_live_trigger(&break_pm, &reset_pm, generation);
	trig_cal_default(&cal);
	cal.full_raw = (int32_t)BIT_MASK(trigger_adc.resolution);
	cal.break_pm = break_pm;
	cal.reset_pm = reset_pm;
	if (!trig_cal_valid(&cal)) {
		LOG_ERR("invalid trigger thresholds %u/%u, using defaults", break_pm, reset_pm);
		trig_cal_default(&cal);
		cal.full_raw = (int32_t)BIT_MASK(trigger_adc.resolution);
	}
	trig_init(&proc, &cal);
}

static void trigger_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint16_t buf;
	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};
	uint32_t cal_generation;
	uint32_t n = 0U;
	int wdt = wdt_register("trigger", TRIGGER_WDT_TIMEOUT_MS);

	(void)adc_sequence_init_dt(&trigger_adc, &seq);
	load_calibration(&cal_generation);

	for (;;) {
		(void)k_sem_take(&sample_sem, K_FOREVER);

		if ((++n % TRIGGER_WDT_FEED_EVERY) == 0U) {
			wdt_feed(wdt);
		}
		if (atomic_get(&stall_requested) != 0) {
			LOG_WRN("trigger thread stalling (debug command)");
			k_sleep(K_FOREVER);
		}

		if (cfg_live_trigger_generation() != cal_generation) {
			load_calibration(&cal_generation);
		}

		/* Sample time: a break is reported with it (control_post_at). */
		uint64_t t_sample = local_clock_us();
		uint32_t c0 = k_cycle_get_32();
		int rc = adc_read_dt(&trigger_adc, &seq);
		uint32_t read_us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
		enum trig_event ev = TRIG_EV_NONE;

		if (rc == 0) {
			ev = trig_update(&proc, (int32_t)buf, (uint32_t)t_sample);
		}

		k_spinlock_key_t key = k_spin_lock(&snap_lock);

		if (read_us > snap.read_us_max) {
			snap.read_us_max = read_us;
		}
		if (rc == 0) {
			snap.raw = (int32_t)buf;
			snap.travel_pm = proc.travel_pm;
			snap.armed = proc.armed;
			snap.last = *trig_last_metrics(&proc);
			snap.samples++;
		} else {
			snap.adc_errors++;
		}
		k_spin_unlock(&snap_lock, key);

		if (ev == TRIG_EV_BREAK) {
			(void)control_post_at(WEV_TRIGGER_BREAK, t_sample);
		} else if (ev == TRIG_EV_RESET) {
			(void)control_post_at(WEV_TRIGGER_RESET, t_sample);
		}
	}
}

int trigger_task_start(void)
{
	if (!adc_is_ready_dt(&trigger_adc)) {
		LOG_ERR("ADC %s not ready", trigger_adc.dev->name);
		return -ENODEV;
	}

	int rc = adc_channel_setup_dt(&trigger_adc);

	if (rc != 0) {
		LOG_ERR("ADC channel setup failed (%d)", rc);
		return rc;
	}

	(void)k_thread_create(&trigger_thread, trigger_stack, K_THREAD_STACK_SIZEOF(trigger_stack),
			      trigger_main, NULL, NULL, NULL, TRIGGER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&trigger_thread, "trigger");

	k_timer_start(&sample_timer, K_USEC(1000000U / CONFIG_TRAINER_TRIGGER_SAMPLE_HZ),
		      K_USEC(1000000U / CONFIG_TRAINER_TRIGGER_SAMPLE_HZ));
	return 0;
}
