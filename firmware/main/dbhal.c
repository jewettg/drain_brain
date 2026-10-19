#include "dbhal.h"
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "dbhal";

/* ---------- inputs ---------- */
typedef struct { int gpio, active_level, debounce_ms; } in_def_t;
static const in_def_t IN[DBH_IN_COUNT] = {
    [DBH_IN_USER]   = {PIN_USER_BTN,  LVL_USER_PRESSED,   DEBOUNCE_USER_MS},
    [DBH_IN_RESET]  = {PIN_RESET_BTN, LVL_RESET_PRESSED,  DEBOUNCE_RESET_MS},
    [DBH_IN_BOTTLE] = {PIN_BOTTLE,    LVL_BOTTLE_PRESENT, DEBOUNCE_BOTTLE_MS},
    [DBH_IN_FLOAT]  = {PIN_FLOAT,     LVL_FLOAT_FLOODED,  DEBOUNCE_FLOAT_MS},
    [DBH_IN_POWER]  = {PIN_EXT_POWER, LVL_EXT_POWER,      DEBOUNCE_POWER_MS},
};
static bool s_state[DBH_IN_COUNT];
static QueueHandle_t s_evq;

static void input_task(void *arg)
{
    int last_raw[DBH_IN_COUNT];
    int64_t since[DBH_IN_COUNT];
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < DBH_IN_COUNT; i++) { last_raw[i] = gpio_get_level(IN[i].gpio); since[i] = now; }
    for (;;) {
        now = esp_timer_get_time() / 1000;
        for (int i = 0; i < DBH_IN_COUNT; i++) {
            int raw = gpio_get_level(IN[i].gpio);
            if (raw != last_raw[i]) { last_raw[i] = raw; since[i] = now; }
            bool logical = raw == IN[i].active_level;
            if (logical != s_state[i] && now - since[i] >= IN[i].debounce_ms) {
                s_state[i] = logical;
                dbh_event_t ev = { .in = i, .state = logical, .ts_ms = now };
                xQueueSend(s_evq, &ev, 0);
            }
        }
        vTaskDelay(1);
    }
}

QueueHandle_t dbh_events(void) { return s_evq; }

dbh_inputs_t dbh_inputs(void)
{
    return (dbh_inputs_t){ s_state[DBH_IN_USER], s_state[DBH_IN_RESET], s_state[DBH_IN_BOTTLE], s_state[DBH_IN_FLOAT], s_state[DBH_IN_POWER] };
}

/* ---------- LEDs ---------- */
static const int LED_PIN[4] = {PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4};
static volatile led_mode_t s_led[4];

void dbh_led(int idx, led_mode_t m) { if (idx >= 0 && idx < 4) s_led[idx] = m; }

static void led_task(void *arg)
{
    for (uint32_t tick = 0;; tick++) {            /* 50 ms steps (5 ticks) */
        for (int i = 0; i < 4; i++) {
            int on = 0;
            switch (s_led[i]) {
            case LED_ON: on = 1; break;
            case LED_SLOW: on = (tick / 10) & 1; break;    /* 1 s period */
            case LED_FAST: on = (tick / 4) & 1; break;     /* 400 ms period */
            default: break;
            }
            gpio_set_level(LED_PIN[i], on);
        }
        vTaskDelay(5);
    }
}

/* ---------- actuator ---------- */
static act_mode_t s_mode = ACT_PULSE_VALVE;
static float s_max_s = 30;
static volatile bool s_busy;
static volatile bool s_cancel;
static float s_last_s;
static TaskHandle_t s_act_task;
static QueueHandle_t s_actq;

void act_set_mode(act_mode_t m) { s_mode = m; }
void act_set_max_seconds(float s) { s_max_s = s > ACT_ABS_MAX_SECONDS ? ACT_ABS_MAX_SECONDS : (s < 1 ? 1 : s); }
bool act_busy(void) { return s_busy; }
float act_last_seconds(void) { return s_last_s; }

static void drive_off(void) { gpio_set_level(PIN_DRV_A, 0); gpio_set_level(PIN_DRV_B, 0); }

static void pulse(int pin)
{
    gpio_set_level(pin == PIN_DRV_A ? PIN_DRV_B : PIN_DRV_A, 0);
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(ACT_PULSE_MS));
    gpio_set_level(pin, 0);
}

void act_boot_safe(void)
{
    if (s_mode == ACT_PULSE_VALVE) pulse(PIN_DRV_B);
}

esp_err_t act_start(float seconds)
{
    if (seconds <= 0 || seconds > s_max_s) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;
    s_cancel = false;
    if (xQueueSend(s_actq, &seconds, 0) != pdTRUE) { s_busy = false; return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}

void act_cancel(void) { s_cancel = true; }

static void act_task(void *arg)
{
    float sec;
    for (;;) {
        if (xQueueReceive(s_actq, &sec, portMAX_DELAY) != pdTRUE) continue;
        int64_t t0 = esp_timer_get_time();
        if (s_mode == ACT_PULSE_VALVE) {
            pulse(PIN_DRV_A);                                   /* open */
            int64_t until = esp_timer_get_time() + (int64_t)(sec * 1e6f);
            while (!s_cancel && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
            pulse(PIN_DRV_B);                                   /* close */
        } else {
            gpio_set_level(PIN_DRV_B, 0);
            gpio_set_level(PIN_DRV_A, 1);
            int64_t until = esp_timer_get_time() + (int64_t)(sec * 1e6f);
            while (!s_cancel && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(PIN_DRV_A, 0);
        }
        drive_off();
        s_last_s = (esp_timer_get_time() - t0) / 1e6f;
        s_busy = false;
    }
}

/* ---------- battery ---------- */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

int dbh_battery_pin_mv(void)
{
    int sum = 0, raw, mv = -1;
    for (int i = 0; i < 16; i++) {
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        sum += raw;
    }
    raw = sum / 16;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return -1;
}

static void adc_setup(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &c));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cc, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cc, &s_cali) == ESP_OK;
#endif
    if (!s_cali_ok) ESP_LOGW(TAG, "ADC calibration unavailable; battery readings disabled");
}

void dbh_init(void)
{
    /* Drive pins low before anything else. */
    gpio_config_t drv = { .pin_bit_mask = (1ULL << PIN_DRV_A) | (1ULL << PIN_DRV_B), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&drv);
    drive_off();

    uint64_t lm = 0;
    for (int i = 0; i < 4; i++) lm |= 1ULL << LED_PIN[i];
    gpio_config_t led = { .pin_bit_mask = lm, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);

    uint64_t im = 0;
    for (int i = 0; i < DBH_IN_COUNT; i++) im |= 1ULL << IN[i].gpio;
    gpio_config_t in = { .pin_bit_mask = im, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    for (int i = 0; i < DBH_IN_COUNT; i++) s_state[i] = gpio_get_level(IN[i].gpio) == IN[i].active_level;

    s_evq = xQueueCreate(16, sizeof(dbh_event_t));
    s_actq = xQueueCreate(1, sizeof(float));
    adc_setup();
    xTaskCreate(input_task, "inputs", 3072, NULL, 6, NULL);
    xTaskCreate(led_task, "leds", 2048, NULL, 3, NULL);
    xTaskCreate(act_task, "actuator", 3072, NULL, 7, &s_act_task);
}
