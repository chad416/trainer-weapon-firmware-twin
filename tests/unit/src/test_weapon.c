/* SPEC F03-F08, R04: weapon state machine. */
#include <zephyr/ztest.h>

#include "weapon_sm.h"

static struct weapon w;
static uint32_t now_ms;

static uint32_t send(enum weapon_event_type type)
{
	struct weapon_event ev = {.type = type, .t_ms = now_ms};

	now_ms += 10U;
	return weapon_handle(&w, &ev);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	now_ms = 0U;
	weapon_init(&w, 30);
}

/* SAFE -> SEMI, magazine in, rack: READY with 30 rounds. */
static void load_semi(void)
{
	(void)send(WEV_SELECTOR_CYCLE);
	(void)send(WEV_MAG_TOGGLE);
	(void)send(WEV_RACK);
	zassert_equal(weapon_state(&w), WS_READY);
}

ZTEST_SUITE(weapon, NULL, NULL, before, NULL, NULL);

ZTEST(weapon, test_powers_up_safe)
{
	zassert_equal(weapon_state(&w), WS_SAFE);
	zassert_equal(w.sel, SEL_SAFE);
}

ZTEST(weapon, test_trigger_blocked_in_safe)
{
	(void)send(WEV_MAG_TOGGLE);
	(void)send(WEV_RACK);
	uint32_t act = send(WEV_TRIGGER_BREAK);

	zassert_false(act & (WACT_FIRE | WACT_DRY_FIRE));
	zassert_equal(w.shots, 0);
	zassert_equal(weapon_state(&w), WS_SAFE, "loading works in SAFE, firing does not");
}

ZTEST(weapon, test_selector_cycle)
{
	(void)send(WEV_SELECTOR_CYCLE);
	zassert_equal(w.sel, SEL_SEMI);
	(void)send(WEV_SELECTOR_CYCLE);
	zassert_equal(w.sel, SEL_AUTO);
	(void)send(WEV_SELECTOR_CYCLE);
	zassert_equal(w.sel, SEL_SAFE);
	zassert_equal(weapon_state(&w), WS_SAFE);
}

ZTEST(weapon, test_empty_and_loaded_are_dry_fire)
{
	(void)send(WEV_SELECTOR_CYCLE);
	zassert_equal(weapon_state(&w), WS_EMPTY);
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_DRY_FIRE);
	(void)send(WEV_TRIGGER_RESET);

	(void)send(WEV_MAG_TOGGLE);
	zassert_equal(weapon_state(&w), WS_LOADED, "magazine in but not racked");
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_DRY_FIRE);
	zassert_equal(w.shots, 0);
}

ZTEST(weapon, test_semi_one_shot_per_break)
{
	load_semi();
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_FIRE);
	zassert_equal(weapon_state(&w), WS_FIRING);
	zassert_false(send(WEV_CADENCE_TICK) & WACT_FIRE, "no cadence in SEMI");
	(void)send(WEV_TRIGGER_RESET);
	zassert_equal(weapon_state(&w), WS_READY);
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_FIRE);
	zassert_equal(w.shots, 2);
	zassert_equal(rounds_total(&w.rounds), 28);
}

ZTEST(weapon, test_auto_cadence)
{
	load_semi();
	(void)send(WEV_SELECTOR_CYCLE); /* AUTO */

	uint32_t act = send(WEV_TRIGGER_BREAK);

	zassert_true(act & WACT_FIRE);
	zassert_true(act & WACT_CADENCE_START);
	for (int i = 0; i < 5; i++) {
		zassert_true(send(WEV_CADENCE_TICK) & WACT_FIRE);
	}
	act = send(WEV_TRIGGER_RESET);
	zassert_true(act & WACT_CADENCE_STOP);
	zassert_equal(weapon_state(&w), WS_READY);
	zassert_equal(w.shots, 6);
}

ZTEST(weapon, test_auto_runs_dry_and_stops_cadence)
{
	weapon_init(&w, 3);
	load_semi();
	(void)send(WEV_SELECTOR_CYCLE); /* AUTO */
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_CADENCE_START);
	(void)send(WEV_CADENCE_TICK);

	uint32_t act = send(WEV_CADENCE_TICK); /* third and last round */

	zassert_true(act & WACT_FIRE);
	zassert_true(act & WACT_CADENCE_STOP);
	zassert_equal(weapon_state(&w), WS_EMPTY);
	zassert_equal(w.shots, 3);
}

/* Regression: firing the last round from READY in AUTO must not start a cadence. */
ZTEST(weapon, test_auto_last_round_does_not_start_cadence)
{
	weapon_init(&w, 1);
	load_semi();
	(void)send(WEV_SELECTOR_CYCLE); /* AUTO */

	uint32_t act = send(WEV_TRIGGER_BREAK);

	zassert_true(act & WACT_FIRE);
	zassert_false(act & WACT_CADENCE_START);
	zassert_equal(weapon_state(&w), WS_EMPTY);
}

ZTEST(weapon, test_safe_while_firing_stops_and_keeps_rounds)
{
	load_semi();
	(void)send(WEV_SELECTOR_CYCLE); /* AUTO */
	(void)send(WEV_TRIGGER_BREAK);

	uint8_t left = rounds_total(&w.rounds);
	uint32_t act = send(WEV_SELECTOR_CYCLE); /* AUTO -> SAFE */

	zassert_true(act & WACT_CADENCE_STOP);
	zassert_equal(weapon_state(&w), WS_SAFE);
	zassert_equal(rounds_total(&w.rounds), left);
}

ZTEST(weapon, test_held_trigger_when_leaving_safe_does_not_fire)
{
	(void)send(WEV_MAG_TOGGLE);
	(void)send(WEV_RACK);
	(void)send(WEV_TRIGGER_BREAK); /* held in SAFE */

	uint32_t act = send(WEV_SELECTOR_CYCLE);

	zassert_false(act & WACT_FIRE);
	zassert_equal(weapon_state(&w), WS_FIRING, "waiting for the trigger to reset");
	(void)send(WEV_TRIGGER_RESET);
	zassert_true(send(WEV_TRIGGER_BREAK) & WACT_FIRE);
}

ZTEST(weapon, test_stoppage_drill)
{
	load_semi();
	now_ms = 1000U;
	zassert_true(send(WEV_STOPPAGE_INJECT) & WACT_STOPPAGE_START);
	zassert_equal(weapon_state(&w), WS_STOPPAGE);

	zassert_false(send(WEV_TRIGGER_BREAK) & (WACT_FIRE | WACT_DRY_FIRE));
	(void)send(WEV_TRIGGER_RESET);

	(void)send(WEV_RACK); /* wrong order: rack before tap */
	zassert_equal(weapon_state(&w), WS_STOPPAGE);

	uint8_t before_clear = rounds_total(&w.rounds);

	(void)send(WEV_TAP);
	now_ms = 3500U;

	uint32_t act = send(WEV_RACK);

	zassert_true(act & WACT_STOPPAGE_CLEAR);
	zassert_equal(weapon_state(&w), WS_READY);
	zassert_equal(w.last_clear_ms, 2500U);
	zassert_equal(rounds_total(&w.rounds), before_clear - 1, "dud round ejected");
}

ZTEST(weapon, test_fault_blocks_fire)
{
	load_semi();
	(void)send(WEV_FAULT_SET);
	zassert_equal(weapon_state(&w), WS_FAULT);
	zassert_false(send(WEV_TRIGGER_BREAK) & WACT_FIRE);
	(void)send(WEV_TRIGGER_RESET);
	(void)send(WEV_FAULT_CLEAR);
	zassert_equal(weapon_state(&w), WS_READY);
}

ZTEST(weapon, test_state_names)
{
	zassert_str_equal(weapon_state_name(WS_READY), "READY");
	zassert_str_equal(weapon_selector_name(SEL_AUTO), "AUTO");
	zassert_str_equal(weapon_state_name(WS_COUNT), "?");
}
