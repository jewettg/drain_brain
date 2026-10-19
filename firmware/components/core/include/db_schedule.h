#pragma once
/* Calendar schedule engine. Pure C, no ESP-IDF dependency (host-testable).
 * Times are LOCAL wall-clock in the process time zone (TZ env / tzset). */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DB_SCHED_MAX_ENTRIES 16
#define DB_SCHED_MAX_BLACKOUT 4

typedef enum { DB_SCHED_WEEKLY = 0, DB_SCHED_INTERVAL, DB_SCHED_MONTHLY, DB_SCHED_ONCE } db_sched_type_t;
typedef enum { DB_MISSED_SKIP = 0, DB_MISSED_RUN_ONCE_ON_BOOT } db_missed_policy_t;

typedef struct { uint16_t y; uint8_t m, d; } db_date_t;   /* calendar date, m=1..12, d=1..31 */

typedef struct {
    uint16_t id;
    bool enabled;
    db_sched_type_t type;
    uint8_t days_mask;        /* weekly: bit0=Sunday .. bit6=Saturday */
    uint16_t every_days;      /* interval: every N days counted from `anchor` */
    db_date_t anchor;         /* interval */
    uint8_t day_of_month;     /* monthly: 1..31, clamped to the month's last day */
    db_date_t date;           /* once */
    uint8_t hour, minute;     /* local wall-clock time of day */
    float ml;                 /* amount: ml (>0) ... */
    float seconds;            /* ... or seconds (>0); exactly one is set */
} db_sched_entry_t;

typedef struct { db_date_t from, to; } db_blackout_t;   /* inclusive */

typedef struct {
    db_sched_entry_t entries[DB_SCHED_MAX_ENTRIES];
    uint8_t n_entries;
    db_blackout_t blackout[DB_SCHED_MAX_BLACKOUT];
    uint8_t n_blackout;
    db_missed_policy_t missed_policy;
} db_schedule_t;

typedef struct { time_t when; uint16_t entry_id; float ml, seconds; } db_sched_run_t;

/* Returns NULL if valid, else a short reason string. */
const char *db_sched_validate(const db_schedule_t *s);

/* Earliest run strictly after `after` (epoch). Returns false if none within ~13 months. */
bool db_sched_next(const db_schedule_t *s, time_t after, db_sched_run_t *out);

/* Fill up to `max` upcoming runs after `after`; returns count. */
int db_sched_upcoming(const db_schedule_t *s, time_t after, db_sched_run_t *out, int max);

/* Local wall-clock -> epoch. If the wall-clock time does not exist (DST gap), returns the
 * first valid instant after the gap. If it occurs twice (DST overlap), returns the first. */
time_t db_local_to_epoch(db_date_t d, int hour, int minute);

int db_days_in_month(int y, int m);
int db_days_from_civil(int y, int m, int d);   /* days since 1970-01-01, timezone independent */
