/*
 * Weapon state machine (Zephyr State Machine Framework, hierarchical).
 *
 *   SAFE
 *   ARMED ── EMPTY | LOADED | READY | FIRING
 *   STOPPAGE
 *   FAULT
 *
 * The caller feeds events and gets back an action mask; it never touches
 * outputs or timers itself. That keeps this module testable on native_sim.
 */
#ifndef TRAINER_WEAPON_SM_H_
#define TRAINER_WEAPON_SM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/smf.h>

#include "rounds.h"

enum weapon_selector {
	SEL_SAFE = 0,
	SEL_SEMI,
	SEL_AUTO,
};

enum weapon_state_id {
	WS_SAFE = 0,
	WS_ARMED, /* parent only, never reported as the current state */
	WS_EMPTY,
	WS_LOADED,
	WS_READY,
	WS_FIRING,
	WS_STOPPAGE,
	WS_FAULT,
	WS_COUNT,
};

enum weapon_event_type {
	WEV_NONE = 0,
	WEV_SELECTOR_CYCLE, /* SW1: SAFE -> SEMI -> AUTO -> SAFE */
	WEV_MAG_TOGGLE,     /* SW2: insert or remove the magazine */
	WEV_RACK,           /* SW3: charging handle */
	WEV_TAP,            /* SW4: tap the magazine (stoppage drill) */
	WEV_TRIGGER_BREAK,
	WEV_TRIGGER_RESET,
	WEV_CADENCE_TICK,     /* AUTO cadence timer expired */
	WEV_STOPPAGE_INJECT,  /* instructor command */
	WEV_FAULT_SET,        /* critical BIT fault */
	WEV_FAULT_CLEAR,      /* BIT passed */
};

struct weapon_event {
	enum weapon_event_type type;
	uint32_t t_ms;
};

/* Action mask returned by weapon_handle(). */
#define WACT_FIRE           (1U << 0) /* pulse emitter + recoil valve, report a shot */
#define WACT_DRY_FIRE       (1U << 1) /* trigger broke on an empty chamber */
#define WACT_CADENCE_START  (1U << 2) /* start the AUTO cadence timer */
#define WACT_CADENCE_STOP   (1U << 3) /* stop the AUTO cadence timer */
#define WACT_STOPPAGE_START (1U << 4)
#define WACT_STOPPAGE_CLEAR (1U << 5)
#define WACT_STATE_CHANGED  (1U << 6)

struct weapon {
	struct smf_ctx ctx; /* must be the first member (SMF_CTX cast) */
	struct rounds rounds;
	enum weapon_selector sel;
	bool trigger_held; /* trigger past break and not yet reset */
	bool tapped;       /* stoppage drill: tap done, rack expected */
	uint32_t shots;    /* shots fired since boot */
	uint32_t stoppage_t_ms;
	uint32_t last_clear_ms; /* duration of the last stoppage clearance */
	struct weapon_event ev; /* event being processed */
	uint32_t actions;       /* actions produced by the event being processed */
};

void weapon_init(struct weapon *w, uint8_t capacity);

/* Process one event. Returns the WACT_* action mask. */
uint32_t weapon_handle(struct weapon *w, const struct weapon_event *ev);

enum weapon_state_id weapon_state(const struct weapon *w);
const char *weapon_state_name(enum weapon_state_id id);
const char *weapon_selector_name(enum weapon_selector sel);

#endif /* TRAINER_WEAPON_SM_H_ */
