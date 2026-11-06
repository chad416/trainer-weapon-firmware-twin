#include "outputs.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cfg_store.h"

LOG_MODULE_REGISTER(outputs, LOG_LEVEL_INF);

static const struct gpio_dt_spec emitter = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_emitter), gpios);
static const struct gpio_dt_spec valve = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_valve), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_red), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_green), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_blue), gpios);

/* ---- shot pulses ----------------------------------------------------------- */

static void emitter_off(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)gpio_pin_set_dt(&emitter, 0);
}

static void valve_off(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)gpio_pin_set_dt(&valve, 0);
}

K_TIMER_DEFINE(emitter_timer, emitter_off, NULL);
K_TIMER_DEFINE(valve_timer, valve_off, NULL);

void outputs_shot(void)
{
	(void)gpio_pin_set_dt(&emitter, 1);
	(void)gpio_pin_set_dt(&valve, 1);
	k_timer_start(&emitter_timer, K_USEC(cfg_live_emitter_us()), K_NO_WAIT);
	k_timer_start(&valve_timer, K_MSEC(cfg_live_valve_ms()), K_NO_WAIT);
}

/* ---- status LED -------------------------------------------------------------- */

enum led_color {
	LED_OFF = 0,
	LED_RED = BIT(0),
	LED_GREEN = BIT(1),
	LED_BLUE = BIT(2),
	LED_AMBER = LED_RED | LED_GREEN,
};

struct led_pattern {
	enum led_color color;
	uint16_t blink_ms; /* 0 = steady */
};

static struct led_pattern pattern_for(enum weapon_state_id state)
{
	switch (state) {
	case WS_SAFE:
		return (struct led_pattern){LED_BLUE, 0U};
	case WS_READY:
	case WS_FIRING:
		return (struct led_pattern){LED_GREEN, 0U};
	case WS_EMPTY:
	case WS_LOADED:
		return (struct led_pattern){LED_GREEN, 500U};
	case WS_STOPPAGE:
		return (struct led_pattern){LED_AMBER, 150U};
	case WS_FAULT:
	default:
		return (struct led_pattern){LED_RED, 0U};
	}
}

static void led_write(enum led_color c)
{
	(void)gpio_pin_set_dt(&led_r, (c & LED_RED) ? 1 : 0);
	(void)gpio_pin_set_dt(&led_g, (c & LED_GREEN) ? 1 : 0);
	(void)gpio_pin_set_dt(&led_b, (c & LED_BLUE) ? 1 : 0);
}

/*
 * All LED writes happen in this one work item, so the state shown and the
 * blink phase are never touched from two threads at once.
 */
static atomic_t requested_state = ATOMIC_INIT(WS_SAFE);

static void led_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_handler);

static void led_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	static enum weapon_state_id shown = WS_COUNT;
	static bool lit;

	enum weapon_state_id state = (enum weapon_state_id)atomic_get(&requested_state);
	struct led_pattern p = pattern_for(state);

	if (state != shown) {
		shown = state;
		lit = true; /* a new state always starts lit */
	} else if (p.blink_ms != 0U) {
		lit = !lit;
	} else {
		lit = true;
	}

	led_write(lit ? p.color : LED_OFF);
	if (p.blink_ms != 0U) {
		(void)k_work_reschedule(&led_work, K_MSEC(p.blink_ms));
	}
}

void outputs_show_state(enum weapon_state_id state)
{
	atomic_set(&requested_state, (atomic_val_t)state);
	(void)k_work_reschedule(&led_work, K_NO_WAIT);
}

/* ---- init ------------------------------------------------------------------- */

int outputs_init(void)
{
	const struct gpio_dt_spec *all[] = {&emitter, &valve, &led_r, &led_g, &led_b};

	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		if (!gpio_is_ready_dt(all[i])) {
			LOG_ERR("GPIO %s not ready", all[i]->port->name);
			return -ENODEV;
		}
		int rc = gpio_pin_configure_dt(all[i], GPIO_OUTPUT_INACTIVE);

		if (rc != 0) {
			LOG_ERR("GPIO configure failed (%d)", rc);
			return rc;
		}
	}
	return 0;
}
