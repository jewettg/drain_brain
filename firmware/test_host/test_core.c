/* Host tests for the pure-C core. Build: see Makefile. Uses America/Chicago POSIX TZ. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "db_schedule.h"
#include "db_bottle.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static time_t at(int y, int m, int d, int hh, int mm) { return db_local_to_epoch((db_date_t){y, m, d}, hh, mm); }
static void parts(time_t t, int *y, int *mo, int *d, int *h, int *mi)
{
    struct tm l; localtime_r(&t, &l);
    *y = l.tm_year + 1900; *mo = l.tm_mon + 1; *d = l.tm_mday; *h = l.tm_hour; *mi = l.tm_min;
}

static db_sched_entry_t weekly(uint16_t id, uint8_t mask, int h, int m)
{
    db_sched_entry_t e = {0};
    e.id = id; e.enabled = true; e.type = DB_SCHED_WEEKLY; e.days_mask = mask; e.hour = h; e.minute = m; e.ml = 10;
    return e;
}

static void test_calendar(void)
{
    CHECK(db_days_in_month(2024, 2) == 29);
    CHECK(db_days_in_month(2100, 2) == 28);
    CHECK(db_days_in_month(2000, 2) == 29);
    CHECK(db_days_from_civil(1970, 1, 1) == 0);
    CHECK(db_days_from_civil(2024, 3, 1) - db_days_from_civil(2024, 2, 28) == 2);   /* leap year */
}

static void test_weekly_and_dst(void)
{
    int y, mo, d, h, mi;
    db_schedule_t s = {0};
    /* every day at 02:30: spring-forward 2026-03-08 has no 02:30 */
    s.entries[0] = weekly(1, 0x7F, 2, 30); s.n_entries = 1;
    CHECK(db_sched_validate(&s) == NULL);
    db_sched_run_t r;
    CHECK(db_sched_next(&s, at(2026, 3, 7, 0, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(y == 2026 && mo == 3 && d == 7 && h == 2 && mi == 30);
    CHECK(db_sched_next(&s, r.when, &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(mo == 3 && d == 8 && h == 3 && mi == 0);          /* gap -> 03:00 */
    /* fall-back 2026-11-01: 01:30 happens twice; run exactly once */
    s.entries[0] = weekly(1, 0x7F, 1, 30);
    time_t t = at(2026, 10, 31, 12, 0); int count = 0;
    for (int i = 0; i < 4; i++) {
        CHECK(db_sched_next(&s, t, &r)); t = r.when;
        parts(t, &y, &mo, &d, &h, &mi);
        if (mo == 11 && d == 1) count++;
    }
    CHECK(count == 1);
    /* weekly mask: Monday (bit1) and Thursday (bit4); 2026-10-19 is a Monday */
    s.entries[0] = weekly(1, (1 << 1) | (1 << 4), 9, 0);
    CHECK(db_sched_next(&s, at(2026, 10, 19, 9, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(mo == 10 && d == 22 && h == 9);                    /* next Thursday */
}

static void test_other_types(void)
{
    int y, mo, d, h, mi;
    db_sched_run_t r;
    db_schedule_t s = {0};
    db_sched_entry_t e = {0};
    e.id = 2; e.enabled = true; e.type = DB_SCHED_INTERVAL; e.every_days = 3; e.anchor = (db_date_t){2026, 1, 1}; e.hour = 6; e.minute = 30; e.seconds = 4;
    s.entries[0] = e; s.n_entries = 1;
    CHECK(db_sched_next(&s, at(2026, 1, 1, 7, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(mo == 1 && d == 4 && h == 6 && mi == 30);
    /* monthly 31st clamps to short months and leap Feb */
    e = (db_sched_entry_t){0};
    e.id = 3; e.enabled = true; e.type = DB_SCHED_MONTHLY; e.day_of_month = 31; e.hour = 8; e.ml = 5;
    s.entries[0] = e;
    CHECK(db_sched_next(&s, at(2028, 2, 1, 0, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(y == 2028 && mo == 2 && d == 29);                  /* leap year */
    CHECK(db_sched_next(&s, at(2027, 2, 1, 0, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(y == 2027 && mo == 2 && d == 28);
    /* once, in the past -> none */
    e = (db_sched_entry_t){0};
    e.id = 4; e.enabled = true; e.type = DB_SCHED_ONCE; e.date = (db_date_t){2026, 12, 24}; e.hour = 7; e.ml = 5;
    s.entries[0] = e;
    CHECK(db_sched_next(&s, at(2026, 12, 1, 0, 0), &r));
    CHECK(!db_sched_next(&s, at(2026, 12, 25, 0, 0), &r));
    /* blackout skips */
    s.entries[0] = weekly(1, 0x7F, 9, 0);
    s.blackout[0] = (db_blackout_t){{2026, 7, 1}, {2026, 7, 14}}; s.n_blackout = 1;
    CHECK(db_sched_next(&s, at(2026, 6, 30, 10, 0), &r));
    parts(r.when, &y, &mo, &d, &h, &mi);
    CHECK(mo == 7 && d == 15);
    /* validation */
    s.entries[0] = weekly(1, 0, 9, 0);
    CHECK(db_sched_validate(&s) != NULL);
    s.entries[0] = weekly(1, 1, 9, 0); s.entries[0].seconds = 3;
    CHECK(db_sched_validate(&s) != NULL);                    /* both ml and seconds */
    /* upcoming list is ascending */
    s.n_blackout = 0; s.entries[0] = weekly(1, 0x7F, 9, 0); s.entries[0].seconds = 0;
    db_sched_run_t list[5];
    CHECK(db_sched_upcoming(&s, at(2026, 10, 19, 0, 0), list, 5) == 5);
    for (int i = 1; i < 5; i++) CHECK(list[i].when > list[i - 1].when);
}

static void test_bottle(void)
{
    db_bottle_t b; db_bottle_defaults(&b); b.present = true;
    CHECK(db_bottle_state(&b) == DB_BOTTLE_FULL);
    CHECK(fabsf(db_bottle_ml_to_seconds(&b, 10) - 10.0f) < 1e-3);
    CHECK(db_bottle_calibrate(&b, 30, 24));                  /* 0.8 ml/s */
    CHECK(fabsf(db_bottle_flow_now(&b) - 0.8f) < 1e-3);
    db_bottle_consume(&b, 400);
    CHECK(db_bottle_state(&b) == DB_BOTTLE_LOW);
    db_bottle_consume(&b, 80);
    CHECK(db_bottle_state(&b) == DB_BOTTLE_EMPTY);
    db_bottle_consume(&b, 1000);
    CHECK(b.remaining_ml == 0);
    db_bottle_reset_full(&b, 0);
    CHECK(b.remaining_ml == 500 && db_bottle_state(&b) == DB_BOTTLE_FULL);
    b.flow_ml_s_100 = 1.0f; b.flow_ml_s_50 = 0.8f; b.flow_ml_s_10 = 0.5f;
    b.remaining_ml = 250;                                    /* 50% */
    CHECK(fabsf(db_bottle_flow_now(&b) - 0.8f) < 1e-3);
    b.remaining_ml = 375;                                    /* 75% */
    CHECK(fabsf(db_bottle_flow_now(&b) - 0.9f) < 1e-3);
    b.present = false;
    CHECK(db_bottle_state(&b) == DB_BOTTLE_MISSING);
    CHECK(db_bottle_doses_left(&b, 10) == 37);
}

static void test_battery(void)
{
    CHECK(db_batt_pack_mv(1000, 237) == 2370);
    CHECK(db_batt_percent(2200, 2200, 3000) == 0);
    CHECK(db_batt_percent(3000, 2200, 3000) == 100);
    CHECK(db_batt_percent(2600, 2200, 3000) == 50);
    db_batt_level_t l = DB_BATT_OK;
    l = db_batt_level(l, 19, 20, 10); CHECK(l == DB_BATT_LOW);
    l = db_batt_level(l, 22, 20, 10); CHECK(l == DB_BATT_LOW);   /* hysteresis */
    l = db_batt_level(l, 26, 20, 10); CHECK(l == DB_BATT_OK);
    l = db_batt_level(l, 9, 20, 10);  CHECK(l == DB_BATT_CRITICAL);
}

int main(void)
{
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();
    test_calendar(); test_weekly_and_dst(); test_other_types(); test_bottle(); test_battery();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
