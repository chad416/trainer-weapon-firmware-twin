#include "rounds.h"

static void feed_from_mag(struct rounds *r)
{
	if (r->mag_in && (r->mag_rounds > 0U)) {
		r->mag_rounds--;
		r->chambered = true;
	}
}

void rounds_init(struct rounds *r, uint8_t capacity)
{
	r->capacity = capacity;
	r->mag_rounds = 0U;
	r->mag_in = false;
	r->chambered = false;
}

void rounds_insert_mag(struct rounds *r)
{
	if (r->mag_in) {
		return;
	}
	r->mag_in = true;
	r->mag_rounds = r->capacity;
}

void rounds_remove_mag(struct rounds *r)
{
	r->mag_in = false;
	r->mag_rounds = 0U;
}

bool rounds_rack(struct rounds *r)
{
	bool ejected = r->chambered;

	r->chambered = false;
	feed_from_mag(r);

	return ejected;
}

bool rounds_fire(struct rounds *r)
{
	if (!r->chambered) {
		return false;
	}

	r->chambered = false;
	feed_from_mag(r);

	return true;
}

uint8_t rounds_total(const struct rounds *r)
{
	return (uint8_t)(r->mag_rounds + (r->chambered ? 1U : 0U));
}
