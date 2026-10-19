#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum { DBH_IN_USER = 0, DBH_IN_RESET, DBH_IN_BOTTLE, DBH_IN_FLOAT, DBH_IN_POWER, DBH_IN_COUNT } dbh_in_t;
typedef struct { dbh_in_t in; bool state; int64_t ts_ms; } dbh_event_t;   /* state = logical (pressed / present / flooded / external power) */

typedef struct { bool user_pressed, reset_pressed, bottle_present, flooded, ext_power; } dbh_inputs_t;

void dbh_init(void);                     /* GPIO, LEDs, ADC, tasks. Drive pins are forced low first. */
QueueHandle_t dbh_events(void);          /* debounced logical input changes */
dbh_inputs_t dbh_inputs(void);

typedef enum { LED_OFF = 0, LED_ON, LED_SLOW, LED_FAST } led_mode_t;
void dbh_led(int idx /*0..3 = LED1..4*/, led_mode_t mode);

typedef enum { ACT_PULSE_VALVE = 0, ACT_MOTOR = 1 } act_mode_t;
void act_set_mode(act_mode_t m);
void act_set_max_seconds(float s);
void act_boot_safe(void);                /* pulse_valve mode: close pulse at boot, like stock */
esp_err_t act_start(float seconds);      /* async; ESP_ERR_INVALID_STATE if busy, ESP_ERR_INVALID_ARG if over the cap */
void act_cancel(void);
bool act_busy(void);
/* Result of the most recent job (valid when !act_busy()). */
float act_last_seconds(void);

int dbh_battery_pin_mv(void);            /* averaged, -1 on error */
