#pragma once
/* Bottle volume model and battery math. Pure C, host-testable. */
#include <stdbool.h>
#include <stdint.h>

typedef enum { DB_BOTTLE_MISSING = 0, DB_BOTTLE_FULL, DB_BOTTLE_OK, DB_BOTTLE_LOW, DB_BOTTLE_EMPTY } db_bottle_state_t;

typedef struct {
    float capacity_ml;
    float remaining_ml;
    float flow_ml_s_100;     /* flow at 100% fill; also used alone when the others are 0 */
    float flow_ml_s_50;      /* optional correction points (0 = unused) */
    float flow_ml_s_10;
    uint8_t low_percent;
    uint8_t empty_percent;
    bool present;
} db_bottle_t;

void db_bottle_defaults(db_bottle_t *b);
float db_bottle_percent(const db_bottle_t *b);
db_bottle_state_t db_bottle_state(const db_bottle_t *b);
float db_bottle_flow_now(const db_bottle_t *b);                 /* ml/s at current fill */
float db_bottle_ml_to_seconds(const db_bottle_t *b, float ml);  /* drive time needed */
float db_bottle_seconds_to_ml(const db_bottle_t *b, float sec);
void db_bottle_consume(db_bottle_t *b, float ml);               /* clamps at 0 */
void db_bottle_reset_full(db_bottle_t *b, float capacity_ml);   /* capacity_ml <= 0 keeps current capacity */
int db_bottle_doses_left(const db_bottle_t *b, float typical_dose_ml);
/* Calibration: seconds run -> measured ml. Returns false on bad input. */
bool db_bottle_calibrate(db_bottle_t *b, float seconds, float measured_ml);

typedef enum { DB_BATT_OK = 0, DB_BATT_LOW, DB_BATT_CRITICAL } db_batt_level_t;
/* Pack millivolts from ADC pin millivolts and the board divider factor (x100, e.g. 237). */
int db_batt_pack_mv(int pin_mv, int factor_x100);
int db_batt_percent(int pack_mv, int min_mv, int max_mv);
/* Hysteresis: drops at <= low/crit thresholds, recovers only 5 points above. */
db_batt_level_t db_batt_level(db_batt_level_t prev, int percent, int low_pct, int crit_pct);
