#include "db_bottle.h"

void db_bottle_defaults(db_bottle_t *b)
{
    b->capacity_ml = 500.0f;
    b->remaining_ml = 500.0f;
    b->flow_ml_s_100 = 1.0f;     /* placeholder until the calibration wizard is run */
    b->flow_ml_s_50 = 0;
    b->flow_ml_s_10 = 0;
    b->low_percent = 20;
    b->empty_percent = 5;
    b->present = false;
}

float db_bottle_percent(const db_bottle_t *b)
{
    if (b->capacity_ml <= 0) return 0;
    float p = 100.0f * b->remaining_ml / b->capacity_ml;
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

db_bottle_state_t db_bottle_state(const db_bottle_t *b)
{
    if (!b->present) return DB_BOTTLE_MISSING;
    float p = db_bottle_percent(b);
    if (p <= b->empty_percent) return DB_BOTTLE_EMPTY;
    if (p <= b->low_percent) return DB_BOTTLE_LOW;
    if (p >= 99.5f) return DB_BOTTLE_FULL;
    return DB_BOTTLE_OK;
}

float db_bottle_flow_now(const db_bottle_t *b)
{
    float p = db_bottle_percent(b);
    float f100 = b->flow_ml_s_100;
    float f50 = b->flow_ml_s_50 > 0 ? b->flow_ml_s_50 : f100;
    float f10 = b->flow_ml_s_10 > 0 ? b->flow_ml_s_10 : f50;
    if (p >= 100) return f100;
    if (p >= 50) return f50 + (f100 - f50) * (p - 50) / 50.0f;
    if (p >= 10) return f10 + (f50 - f10) * (p - 10) / 40.0f;
    return f10;
}

float db_bottle_ml_to_seconds(const db_bottle_t *b, float ml)
{
    float f = db_bottle_flow_now(b);
    return f > 0 ? ml / f : 0;
}

float db_bottle_seconds_to_ml(const db_bottle_t *b, float sec)
{
    return sec * db_bottle_flow_now(b);
}

void db_bottle_consume(db_bottle_t *b, float ml)
{
    b->remaining_ml -= ml;
    if (b->remaining_ml < 0) b->remaining_ml = 0;
}

void db_bottle_reset_full(db_bottle_t *b, float capacity_ml)
{
    if (capacity_ml > 0) b->capacity_ml = capacity_ml;
    b->remaining_ml = b->capacity_ml;
}

int db_bottle_doses_left(const db_bottle_t *b, float typical_dose_ml)
{
    if (typical_dose_ml <= 0) return 0;
    return (int)(b->remaining_ml / typical_dose_ml);
}

bool db_bottle_calibrate(db_bottle_t *b, float seconds, float measured_ml)
{
    if (seconds <= 0 || measured_ml <= 0) return false;
    float f = measured_ml / seconds;
    /* Calibration is taken at the current fill level; scale the table so the curve passes through it. */
    float cur = db_bottle_flow_now(b);
    float k = cur > 0 ? f / cur : 1.0f;
    b->flow_ml_s_100 *= k;
    if (b->flow_ml_s_50 > 0) b->flow_ml_s_50 *= k;
    if (b->flow_ml_s_10 > 0) b->flow_ml_s_10 *= k;
    if (cur <= 0) b->flow_ml_s_100 = f;
    return true;
}

int db_batt_pack_mv(int pin_mv, int factor_x100)
{
    return (pin_mv * factor_x100 + 50) / 100;
}

int db_batt_percent(int pack_mv, int min_mv, int max_mv)
{
    if (max_mv <= min_mv) return 0;
    if (pack_mv <= min_mv) return 0;
    if (pack_mv >= max_mv) return 100;
    return (pack_mv - min_mv) * 100 / (max_mv - min_mv);
}

db_batt_level_t db_batt_level(db_batt_level_t prev, int percent, int low_pct, int crit_pct)
{
    if (percent <= crit_pct) return DB_BATT_CRITICAL;
    if (percent <= low_pct) return prev == DB_BATT_CRITICAL && percent <= crit_pct + 5 ? DB_BATT_CRITICAL : DB_BATT_LOW;
    if (prev != DB_BATT_OK && percent <= low_pct + 5) return prev;
    return DB_BATT_OK;
}
