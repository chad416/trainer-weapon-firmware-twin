#include "weapon_sm.h"

static const struct smf_state weapon_states[WS_COUNT];

/* Leaf state the weapon should be in, from its selector, trigger and rounds. */
static const struct smf_state *settled_state(const struct weapon *w)
{
	if (w->sel == SEL_SAFE) {
		return &weapon_states[WS_SAFE];
	}
	if (w->rounds.chambered) {
		return w->trigger_held ? &weapon_states[WS_FIRING] : &weapon_states[WS_READY];
	}
	if (w->rounds.mag_in && (w->rounds.mag_rounds > 0U)) {
		return &weapon_states[WS_LOADED];
	}
	return &weapon_states[WS_EMPTY];
}

static void go(struct weapon *w, const struct smf_state *target)
{
	if (w->ctx.current != target) {
		smf_set_state(SMF_CTX(w), target);
	}
}

static void settle(struct weapon *w)
{
	go(w, settled_state(w));
}

/* Magazine and charging handle work the same in SAFE and ARMED. */
static bool handle_loading(struct weapon *w)
{
	switch (w->ev.type) {
	case WEV_MAG_TOGGLE:
		if (w->rounds.mag_in) {
			rounds_remove_mag(&w->rounds);
		} else {
			rounds_insert_mag(&w->rounds);
		}
		return true;
	case WEV_RACK:
		(void)rounds_rack(&w->rounds);
		return true;
	default:
		return false;
	}
}

static void fire(struct weapon *w)
{
	if (rounds_fire(&w->rounds)) {
		w->shots++;
		w->actions |= WACT_FIRE;
	} else {
		w->actions |= WACT_DRY_FIRE;
	}
}

/* ---- SAFE ---------------------------------------------------------------- */

static enum smf_state_result safe_run(void *obj)
{
	struct weapon *w = obj;

	if (handle_loading(w)) {
		return SMF_EVENT_HANDLED; /* stay in SAFE */
	}

	switch (w->ev.type) {
	case WEV_SELECTOR_CYCLE:
		w->sel = SEL_SEMI;
		settle(w);
		break;
	case WEV_TRIGGER_BREAK:
		/* Trigger is blocked in SAFE: nothing happens. */
		w->trigger_held = true;
		break;
	case WEV_TRIGGER_RESET:
		w->trigger_held = false;
		break;
	case WEV_FAULT_SET:
		go(w, &weapon_states[WS_FAULT]);
		break;
	default:
		break;
	}
	return SMF_EVENT_HANDLED;
}

/* ---- ARMED (parent of EMPTY, LOADED, READY, FIRING) ---------------------- */

static enum smf_state_result armed_run(void *obj)
{
	struct weapon *w = obj;

	if (handle_loading(w)) {
		settle(w);
		return SMF_EVENT_HANDLED;
	}

	switch (w->ev.type) {
	case WEV_SELECTOR_CYCLE:
		w->sel = (w->sel == SEL_SEMI) ? SEL_AUTO : SEL_SAFE;
		settle(w);
		break;
	case WEV_TRIGGER_RESET:
		w->trigger_held = false;
		settle(w);
		break;
	case WEV_FAULT_SET:
		go(w, &weapon_states[WS_FAULT]);
		break;
	default:
		break;
	}
	return SMF_EVENT_HANDLED;
}

/* EMPTY and LOADED: nothing chambered, so a trigger break is a dry fire. */
static enum smf_state_result unchambered_run(void *obj)
{
	struct weapon *w = obj;

	if (w->ev.type == WEV_TRIGGER_BREAK) {
		w->trigger_held = true;
		w->actions |= WACT_DRY_FIRE;
		return SMF_EVENT_HANDLED;
	}
	return SMF_EVENT_PROPAGATE;
}

static enum smf_state_result ready_run(void *obj)
{
	struct weapon *w = obj;

	switch (w->ev.type) {
	case WEV_TRIGGER_BREAK: {
		w->trigger_held = true;
		fire(w);

		const struct smf_state *target = settled_state(w);

		/* Start the cadence only if a next round is chambered (FIRING).
		 * After the last round the weapon settles in EMPTY and the timer
		 * must not start, because nothing would stop it.
		 */
		if ((w->sel == SEL_AUTO) && (target == &weapon_states[WS_FIRING])) {
			w->actions |= WACT_CADENCE_START;
		}
		go(w, target);
		return SMF_EVENT_HANDLED;
	}
	case WEV_STOPPAGE_INJECT:
		go(w, &weapon_states[WS_STOPPAGE]);
		return SMF_EVENT_HANDLED;
	default:
		return SMF_EVENT_PROPAGATE;
	}
}

static enum smf_state_result firing_run(void *obj)
{
	struct weapon *w = obj;

	switch (w->ev.type) {
	case WEV_CADENCE_TICK:
		if (w->sel == SEL_AUTO) {
			fire(w);
			settle(w); /* out of rounds -> EMPTY/LOADED, which stops the cadence */
		}
		return SMF_EVENT_HANDLED;
	case WEV_STOPPAGE_INJECT:
		go(w, &weapon_states[WS_STOPPAGE]);
		return SMF_EVENT_HANDLED;
	default:
		return SMF_EVENT_PROPAGATE;
	}
}

static void firing_exit(void *obj)
{
	struct weapon *w = obj;

	w->actions |= WACT_CADENCE_STOP;
}

/* ---- STOPPAGE: cleared only by tap, then rack ----------------------------- */

static void stoppage_entry(void *obj)
{
	struct weapon *w = obj;

	w->tapped = false;
	w->stoppage_t_ms = w->ev.t_ms;
	w->actions |= WACT_STOPPAGE_START;
}

static enum smf_state_result stoppage_run(void *obj)
{
	struct weapon *w = obj;

	switch (w->ev.type) {
	case WEV_TAP:
		w->tapped = true;
		break;
	case WEV_RACK:
		if (w->tapped) {
			(void)rounds_rack(&w->rounds); /* eject the dud, chamber a new round */
			w->last_clear_ms = w->ev.t_ms - w->stoppage_t_ms;
			w->actions |= WACT_STOPPAGE_CLEAR;
			settle(w);
		}
		/* Rack before tap: wrong drill order, stoppage stays. */
		break;
	case WEV_MAG_TOGGLE:
		(void)handle_loading(w);
		w->tapped = false; /* the drill restarts after a magazine change */
		break;
	case WEV_SELECTOR_CYCLE:
		w->sel = (enum weapon_selector)((w->sel + 1) % 3);
		break;
	case WEV_TRIGGER_BREAK:
		w->trigger_held = true;
		break;
	case WEV_TRIGGER_RESET:
		w->trigger_held = false;
		break;
	case WEV_FAULT_SET:
		go(w, &weapon_states[WS_FAULT]);
		break;
	default:
		break;
	}
	return SMF_EVENT_HANDLED;
}

/* ---- FAULT: fire blocked until BIT passes --------------------------------- */

static enum smf_state_result fault_run(void *obj)
{
	struct weapon *w = obj;

	switch (w->ev.type) {
	case WEV_FAULT_CLEAR:
		settle(w);
		break;
	case WEV_TRIGGER_BREAK:
		w->trigger_held = true;
		break;
	case WEV_TRIGGER_RESET:
		w->trigger_held = false;
		break;
	default:
		break;
	}
	return SMF_EVENT_HANDLED;
}

static const struct smf_state weapon_states[WS_COUNT] = {
	[WS_SAFE] = SMF_CREATE_STATE(NULL, safe_run, NULL, NULL, NULL),
	[WS_ARMED] = SMF_CREATE_STATE(NULL, armed_run, NULL, NULL, NULL),
	[WS_EMPTY] = SMF_CREATE_STATE(NULL, unchambered_run, NULL, &weapon_states[WS_ARMED], NULL),
	[WS_LOADED] = SMF_CREATE_STATE(NULL, unchambered_run, NULL, &weapon_states[WS_ARMED], NULL),
	[WS_READY] = SMF_CREATE_STATE(NULL, ready_run, NULL, &weapon_states[WS_ARMED], NULL),
	[WS_FIRING] = SMF_CREATE_STATE(NULL, firing_run, firing_exit, &weapon_states[WS_ARMED],
				       NULL),
	[WS_STOPPAGE] = SMF_CREATE_STATE(stoppage_entry, stoppage_run, NULL, NULL, NULL),
	[WS_FAULT] = SMF_CREATE_STATE(NULL, fault_run, NULL, NULL, NULL),
};

void weapon_init(struct weapon *w, uint8_t capacity)
{
	rounds_init(&w->rounds, capacity);
	w->sel = SEL_SAFE;
	w->trigger_held = false;
	w->tapped = false;
	w->shots = 0U;
	w->stoppage_t_ms = 0U;
	w->last_clear_ms = 0U;
	w->ev = (struct weapon_event){.type = WEV_NONE, .t_ms = 0U};
	w->actions = 0U;
	smf_set_initial(SMF_CTX(w), &weapon_states[WS_SAFE]);
}

uint32_t weapon_handle(struct weapon *w, const struct weapon_event *ev)
{
	const struct smf_state *before = w->ctx.current;

	w->ev = *ev;
	w->actions = 0U;
	(void)smf_run_state(SMF_CTX(w));

	if (w->ctx.current != before) {
		w->actions |= WACT_STATE_CHANGED;
	}
	return w->actions;
}

enum weapon_state_id weapon_state(const struct weapon *w)
{
	for (int i = 0; i < WS_COUNT; i++) {
		if (w->ctx.current == &weapon_states[i]) {
			return (enum weapon_state_id)i;
		}
	}
	return WS_FAULT;
}

const char *weapon_state_name(enum weapon_state_id id)
{
	static const char *const names[WS_COUNT] = {
		[WS_SAFE] = "SAFE",       [WS_ARMED] = "ARMED",   [WS_EMPTY] = "EMPTY",
		[WS_LOADED] = "LOADED",   [WS_READY] = "READY",   [WS_FIRING] = "FIRING",
		[WS_STOPPAGE] = "STOPPAGE", [WS_FAULT] = "FAULT",
	};

	return ((unsigned int)id < WS_COUNT) ? names[id] : "?";
}

const char *weapon_selector_name(enum weapon_selector sel)
{
	switch (sel) {
	case SEL_SAFE:
		return "SAFE";
	case SEL_SEMI:
		return "SEMI";
	case SEL_AUTO:
		return "AUTO";
	default:
		return "?";
	}
}
