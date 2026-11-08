#include "outputs.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "cfg_store.h"

LOG_MODULE_REGISTER(outputs, LOG_LEVEL_INF);

static const struct gpio_dt_spec emitter = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_emitter), gpios);
static const struct gpio_dt_spec valve = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_valve), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_red), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_green), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(trainer_status_blue), gpios);

/* ---- shot pulses ----------------------------------------------------------- */

/*
 * The emitter gate has two owners: the direct-mode pulse (control thread) and
 * the camera-mode window (strobe thread). Each has its own timers and holds
 * the gate through its own bit, and the gate is on while either bit is set.
 * A mode change while the other owner's pulse runs can neither cut that pulse
 * short nor stretch it.
 */
#define EMIT_DIRECT BIT(0)
#define EMIT_WINDOW BIT(1)

static struct k_spinlock emitter_lock;
static uint32_t emitter_holders; /* guarded by emitter_lock */

static void emitter_hold(uint32_t owner, bool on)
{
	k_spinlock_key_t key = k_spin_lock(&emitter_lock);

	emitter_holders = on ? (emitter_holders | owner) : (emitter_holders & ~owner);
	(void)gpio_pin_set_dt(&emitter, (emitter_holders != 0U) ? 1 : 0);
	k_spin_unlock(&emitter_lock, key);
}

static void emitter_off(struct k_timer *t)
{
	ARG_UNUSED(t);
	emitter_hold(EMIT_DIRECT, false);
}

static void valve_off(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)gpio_pin_set_dt(&valve, 0);
}

K_TIMER_DEFINE(emitter_timer, emitter_off, NULL);
K_TIMER_DEFINE(valve_timer, valve_off, NULL);

void outputs_emitter_pulse(uint32_t us)
{
	emitter_hold(EMIT_DIRECT, true);
	k_timer_start(&emitter_timer, K_USEC(us), K_NO_WAIT);
}

/*
 * Camera-mode window (v0.6.1). Both edges are switched in the system timer
 * interrupt on absolute tick edges: the on-timer turns the gate on and starts
 * the off-timer for the tick the window ends on. An absolute timeout fires on
 * exactly that tick edge; a relative one started from a thread gets a tick
 * added (Zephyr rounds "at least N ticks" up from inside the current tick),
 * which made the v0.6.0 pulses 0.56-0.61 ms long instead of 0.5 ms.
 * Only the strobe thread calls this, so both window timers have one owner.
 */
static k_ticks_t window_off_tick;  /* written before the on-timer is started */
static uint64_t window_on_cycles;  /* written by the on-timer, read after it expired */

static void window_off(struct k_timer *t)
{
	ARG_UNUSED(t);
	emitter_hold(EMIT_WINDOW, false);
}

K_TIMER_DEFINE(window_off_timer, window_off, NULL);

static void window_on(struct k_timer *t)
{
	ARG_UNUSED(t);
	emitter_hold(EMIT_WINDOW, true);
	window_on_cycles = k_cycle_get_64();
	k_timer_start(&window_off_timer, K_TIMEOUT_ABS_TICKS(window_off_tick), K_NO_WAIT);
}

K_TIMER_DEFINE(window_timer, window_on, NULL);

uint64_t outputs_emitter_window(k_ticks_t on_tick, k_ticks_t off_tick)
{
	window_off_tick = off_tick;
	k_timer_start(&window_timer, K_TIMEOUT_ABS_TICKS(on_tick), K_NO_WAIT);
	(void)k_timer_status_sync(&window_timer);     /* gate on, off-timer started */
	(void)k_timer_status_sync(&window_off_timer); /* window over */
	return window_on_cycles;
}

void outputs_valve(void)
{
	(void)gpio_pin_set_dt(&valve, 1);
	k_timer_start(&valve_timer, K_MSEC(cfg_live_valve_ms()), K_NO_WAIT);
}

void outputs_shot(void)
{
	outputs_emitter_pulse(cfg_live_emitter_us());
	outputs_valve();
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
