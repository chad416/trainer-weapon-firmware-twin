/* SPEC F04, F05, F07: magazine and chamber logic, including "30 + 1". */
#include <zephyr/ztest.h>

#include "rounds.h"

static struct rounds r;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	rounds_init(&r, 30);
}

ZTEST_SUITE(rounds, NULL, NULL, before, NULL, NULL);

ZTEST(rounds, test_init_empty)
{
	zassert_false(r.mag_in);
	zassert_false(r.chambered);
	zassert_equal(rounds_total(&r), 0);
}

ZTEST(rounds, test_insert_gives_full_mag_nothing_chambered)
{
	rounds_insert_mag(&r);
	zassert_true(r.mag_in);
	zassert_equal(r.mag_rounds, 30);
	zassert_false(r.chambered);
}

ZTEST(rounds, test_insert_twice_is_ignored)
{
	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	rounds_insert_mag(&r);
	zassert_equal(r.mag_rounds, 29, "second insert must not refill");
}

ZTEST(rounds, test_rack_chambers_from_mag)
{
	rounds_insert_mag(&r);
	zassert_false(rounds_rack(&r), "nothing to eject");
	zassert_true(r.chambered);
	zassert_equal(r.mag_rounds, 29);
	zassert_equal(rounds_total(&r), 30);
}

ZTEST(rounds, test_rack_with_round_chambered_ejects_it)
{
	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	zassert_true(rounds_rack(&r), "chambered round must be ejected");
	zassert_true(r.chambered);
	zassert_equal(rounds_total(&r), 29);
}

ZTEST(rounds, test_fire_auto_feeds)
{
	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	zassert_true(rounds_fire(&r));
	zassert_true(r.chambered);
	zassert_equal(rounds_total(&r), 29);
}

ZTEST(rounds, test_full_mag_gives_exactly_capacity_shots)
{
	int shots = 0;

	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	while (rounds_fire(&r)) {
		shots++;
	}
	zassert_equal(shots, 30);
	zassert_false(r.chambered);
	zassert_false(rounds_fire(&r), "empty chamber is a dry fire");
}

ZTEST(rounds, test_remove_mag_keeps_chambered_round)
{
	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	rounds_remove_mag(&r);
	zassert_true(r.chambered);
	zassert_equal(rounds_total(&r), 1);
}

ZTEST(rounds, test_thirty_plus_one)
{
	rounds_insert_mag(&r);
	(void)rounds_rack(&r);
	rounds_remove_mag(&r);
	rounds_insert_mag(&r);
	zassert_equal(rounds_total(&r), 31);
}
