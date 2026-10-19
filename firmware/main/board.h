#pragma once
/* Board map for the iFLO Pro main board QAT1P1_V1.1 (PCB variant 3), from documentation/HARDWARE_MAP.md.
 * Every line marked ASSUMPTION is unverified until the Phase 0 probe results are in. Change it here only. */

/* Inputs (internal pull-up on). */
#define PIN_USER_BTN    6    /* ASSUMPTION: GPIO6 is the wake/dispense button; physical switch unconfirmed */
#define PIN_RESET_BTN   17   /* ASSUMPTION: GPIO17 is the reset button; physical switch unconfirmed */
#define PIN_BOTTLE      13
#define PIN_FLOAT       7
#define PIN_EXT_POWER   16

/* Logical meaning of the pin level. */
#define LVL_USER_PRESSED     0
#define LVL_RESET_PRESSED    0
#define LVL_BOTTLE_PRESENT   0   /* ASSUMPTION: LOW = bottle present */
#define LVL_FLOAT_FLOODED    0   /* ASSUMPTION: LOW = flooded */
#define LVL_EXT_POWER        1   /* ASSUMPTION: HIGH = adapter present */

/* Debounce (ms), from the stock timings. */
#define DEBOUNCE_USER_MS     50
#define DEBOUNCE_RESET_MS    50
#define DEBOUNCE_BOTTLE_MS   3000
#define DEBOUNCE_FLOAT_MS    2000
#define DEBOUNCE_POWER_MS    1000

/* Outputs (active-high). LED order: LED1..LED4. */
#define PIN_LED1 40          /* ASSUMPTION: green */
#define PIN_LED2 39          /* ASSUMPTION: blue  */
#define PIN_LED3 41          /* ASSUMPTION: amber */
#define PIN_LED4 42          /* red (confirmed by stock firmware's own name) */
#define PIN_DRV_A 9          /* INA1 */
#define PIN_DRV_B 10         /* INB1 */

/* Actuator. ASSUMPTION: bidirectional valve driven by ~500 ms pulses (A=open, B=close). */
#define ACT_PULSE_MS 500
#define ACT_ABS_MAX_SECONDS 60   /* hard cap no config can exceed */

/* Battery sense: GPIO8 = ADC1 channel 7. ASSUMPTION: pack mV = pin mV * 2.37, 2.2-3.0 V range (2xAA). */
#define PIN_BATT_ADC 8
#define BATT_ADC_CHANNEL 7
#define BATT_FACTOR_X100 237
