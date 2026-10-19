#include "db_schedule.h"
#include <string.h>

int db_days_in_month(int y, int m)
{
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

/* Howard Hinnant's days_from_civil */
int db_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static int weekday_of(db_date_t d)   /* 0 = Sunday */
{
    int z = db_days_from_civil(d.y, d.m, d.d);
    int w = (z + 4) % 7;             /* 1970-01-01 was a Thursday (4) */
    return w < 0 ? w + 7 : w;
}

static int date_cmp(db_date_t a, db_date_t b)
{
    if (a.y != b.y) return a.y < b.y ? -1 : 1;
    if (a.m != b.m) return a.m < b.m ? -1 : 1;
    if (a.d != b.d) return a.d < b.d ? -1 : 1;
    return 0;
}

static db_date_t date_add_days(db_date_t d, int n)
{
    int dim;
    while (n-- > 0) {
        dim = db_days_in_month(d.y, d.m);
        if (++d.d > dim) { d.d = 1; if (++d.m > 12) { d.m = 1; d.y++; } }
    }
    return d;
}

time_t db_local_to_epoch(db_date_t d, int hour, int minute)
{
    struct tm tm = {0};
    tm.tm_year = d.y - 1900; tm.tm_mon = d.m - 1; tm.tm_mday = d.d;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    struct tm chk;
    localtime_r(&t, &chk);
    if (chk.tm_mday == d.d && chk.tm_hour == hour && chk.tm_min == minute) {
        /* Overlap: make sure we return the FIRST occurrence. */
        time_t prev = t - 3600;
        struct tm p;
        localtime_r(&prev, &p);
        if (p.tm_mday == d.d && p.tm_hour == hour && p.tm_min == minute) return prev;
        return t;
    }
    /* Gap (or normalized elsewhere): walk back to the first instant at/after the missing wall time. */
    int want = hour * 60 + minute;
    for (int i = 0; i < 180; i++) {
        time_t q = t - 60;
        struct tm c;
        localtime_r(&q, &c);
        if (c.tm_mday != d.d || c.tm_hour * 60 + c.tm_min < want) break;
        t = q;
    }
    return t;
}

static bool in_blackout(const db_schedule_t *s, db_date_t d)
{
    for (int i = 0; i < s->n_blackout; i++)
        if (date_cmp(d, s->blackout[i].from) >= 0 && date_cmp(d, s->blackout[i].to) <= 0) return true;
    return false;
}

static bool applies_on(const db_sched_entry_t *e, db_date_t d)
{
    switch (e->type) {
    case DB_SCHED_WEEKLY:
        return (e->days_mask >> weekday_of(d)) & 1;
    case DB_SCHED_INTERVAL: {
        int diff = db_days_from_civil(d.y, d.m, d.d) - db_days_from_civil(e->anchor.y, e->anchor.m, e->anchor.d);
        return diff >= 0 && e->every_days > 0 && diff % e->every_days == 0;
    }
    case DB_SCHED_MONTHLY: {
        int dim = db_days_in_month(d.y, d.m);
        int want = e->day_of_month > dim ? dim : e->day_of_month;
        return d.d == want;
    }
    case DB_SCHED_ONCE:
        return date_cmp(d, e->date) == 0;
    }
    return false;
}

const char *db_sched_validate(const db_schedule_t *s)
{
    if (s->n_entries > DB_SCHED_MAX_ENTRIES) return "too many entries";
    if (s->n_blackout > DB_SCHED_MAX_BLACKOUT) return "too many blackout ranges";
    for (int i = 0; i < s->n_entries; i++) {
        const db_sched_entry_t *e = &s->entries[i];
        if (e->hour > 23 || e->minute > 59) return "bad time of day";
        if ((e->ml > 0) == (e->seconds > 0)) return "set exactly one of ml or seconds";
        if (e->type == DB_SCHED_WEEKLY && (e->days_mask & 0x7F) == 0) return "weekly entry needs at least one day";
        if (e->type == DB_SCHED_INTERVAL && (e->every_days < 1 || e->every_days > 365)) return "every_days must be 1..365";
        if (e->type == DB_SCHED_MONTHLY && (e->day_of_month < 1 || e->day_of_month > 31)) return "day_of_month must be 1..31";
        for (int j = 0; j < i; j++) if (s->entries[j].id == e->id) return "duplicate entry id";
    }
    return NULL;
}

bool db_sched_next(const db_schedule_t *s, time_t after, db_sched_run_t *out)
{
    struct tm lt;
    localtime_r(&after, &lt);
    db_date_t start = { (uint16_t)(lt.tm_year + 1900), (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_mday };
    bool found = false;
    for (int i = 0; i < s->n_entries; i++) {
        const db_sched_entry_t *e = &s->entries[i];
        if (!e->enabled) continue;
        db_date_t d = start;
        for (int day = 0; day < 400; day++, d = date_add_days(d, 1)) {
            if (!applies_on(e, d) || in_blackout(s, d)) continue;
            time_t t = db_local_to_epoch(d, e->hour, e->minute);
            if (t <= after) continue;
            if (!found || t < out->when) {
                out->when = t; out->entry_id = e->id; out->ml = e->ml; out->seconds = e->seconds;
                found = true;
            }
            break;   /* earliest for this entry */
        }
    }
    return found;
}

int db_sched_upcoming(const db_schedule_t *s, time_t after, db_sched_run_t *out, int max)
{
    int n = 0;
    while (n < max && db_sched_next(s, after, &out[n])) { after = out[n].when; n++; }
    return n;
}
