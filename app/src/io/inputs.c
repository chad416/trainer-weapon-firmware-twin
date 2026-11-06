/*
 * Buttons -> weapon events.
 * SW1 selector, SW2 magazine, SW3 charging handle (rack), SW4 tap.
 * The board's gpio-keys node reports them as INPUT_KEY_1..INPUT_KEY_4.
 */
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "control.h"

LOG_MODULE_REGISTER(inputs, LOG_LEVEL_INF);

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if ((evt->type != INPUT_EV_KEY) || (evt->value != 1)) {
		return; /* act on press only */
	}

	enum weapon_event_type type;

	switch (evt->code) {
	case INPUT_KEY_1:
		type = WEV_SELECTOR_CYCLE;
		break;
	case INPUT_KEY_2:
		type = WEV_MAG_TOGGLE;
		break;
	case INPUT_KEY_3:
		type = WEV_RACK;
		break;
	case INPUT_KEY_4:
		type = WEV_TAP;
		break;
	default:
		return;
	}

	if (control_post(type) != 0) {
		LOG_WRN("event queue full, input dropped");
	}
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);
