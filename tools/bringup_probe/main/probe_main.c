/*
 * drain_brain bring-up probe (ESP32-S2, iFLO Pro PCB variant 3).
 *
 * Read-only by default. Outputs that can move the pump/valve are locked until
 * you type `arm`, and every drive command has a hard time limit.
 * See README.md and documentation/BRINGUP_INSTRUCTIONS.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_chip_info.h"

#define PROBE_VERSION "0.1.0"

/* Pin map from documentation/HARDWARE_MAP.md. Names say what we believe; the probe exists to prove it. */
typedef struct { int gpio; const char *name; } pin_t;
static const pin_t INPUTS[] = {
    {6,  "GPIO6  user/wake button?"},
    {17, "GPIO17 reset button?"},
    {13, "GPIO13 bottle switch (blue)"},
    {7,  "GPIO7  float/PIP"},
    {16, "GPIO16 external power"},
    {19, "GPIO19 PCB strap"},
    {20, "GPIO20 PCB strap"},
};
#define N_IN (sizeof(INPUTS) / sizeof(INPUTS[0]))
static const int LED_GPIO[4] = {40, 39, 41, 42};      /* LED1..LED4 (green, blue, amber, red) */
#define GPIO_INA 9                                    /* motor/valve driver INA1 */
#define GPIO_INB 10                                   /* motor/valve driver INB1 */

#define PULSE_MAX_MS 600
#define HOLD_MAX_MS 3000
#define ARM_TIMEOUT_US (60LL * 1000000LL)

static volatile bool s_watch = true;
static volatile int s_level[N_IN];
static volatile uint32_t s_edges[N_IN];
static volatile int64_t s_last_change_us[N_IN];
static int64_t s_armed_until_us;
static int s_cur_gpio = -1;      /* GPIO used as current sense while driving, -1 = none */
static int s_cur_limit_mv;       /* abort drive above this (0 = disabled) */

/* ---------- ADC (ADC1, GPIO1..GPIO8 = channels 0..7; GPIO9/10 are the motor pins and are skipped) ---------- */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

static void adc_setup(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    for (int ch = 0; ch <= 7; ch++) ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ch, &c));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cc, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_13 };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cc, &s_cali) == ESP_OK;
#endif
}

/* Average of 16 samples; returns raw, writes millivolts (or -1 if no calibration). */
static int adc_read_gpio(int gpio, int *mv)
{
    if (gpio < 1 || gpio > 8) { *mv = -1; return -1; }
    int ch = gpio - 1, sum = 0, raw;
    for (int i = 0; i < 16; i++) {
        if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) { *mv = -1; return -1; }
        sum += raw;
    }
    raw = sum / 16;
    *mv = -1;
    if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, mv);
    return raw;
}

/* ---------- GPIO setup ---------- */
static void io_setup(void)
{
    /* Motor/valve inputs first, held low, before anything else. */
    gpio_config_t drv = { .pin_bit_mask = (1ULL << GPIO_INA) | (1ULL << GPIO_INB), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&drv);
    gpio_set_level(GPIO_INA, 0);
    gpio_set_level(GPIO_INB, 0);

    uint64_t led_mask = 0;
    for (int i = 0; i < 4; i++) led_mask |= 1ULL << LED_GPIO[i];
    gpio_config_t led = { .pin_bit_mask = led_mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);
    for (int i = 0; i < 4; i++) gpio_set_level(LED_GPIO[i], 0);

    uint64_t in_mask = 0;
    for (size_t i = 0; i < N_IN; i++) in_mask |= 1ULL << INPUTS[i].gpio;
    gpio_config_t in = { .pin_bit_mask = in_mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    for (size_t i = 0; i < N_IN; i++) s_level[i] = gpio_get_level(INPUTS[i].gpio);
}

static void drive_off(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_level(GPIO_INA, 0);
    gpio_set_level(GPIO_INB, 0);
}

/* ---------- input watcher ---------- */
static void watch_task(void *arg)
{
    int last[N_IN];
    for (size_t i = 0; i < N_IN; i++) last[i] = s_level[i];
    for (;;) {
        for (size_t i = 0; i < N_IN; i++) {
            int v = gpio_get_level(INPUTS[i].gpio);
            if (v != last[i]) {
                last[i] = v;
                s_level[i] = v;
                s_edges[i]++;
                s_last_change_us[i] = esp_timer_get_time();
                if (s_watch)
                    printf("[%8lld ms] %-30s -> %s\n", (long long)(s_last_change_us[i] / 1000), INPUTS[i].name, v ? "HIGH" : "LOW");
            }
        }
        vTaskDelay(1);   /* 10 ms at 100 Hz tick */
    }
}

/* ---------- helpers ---------- */
static bool armed(void)
{
    if (esp_timer_get_time() < s_armed_until_us) return true;
    printf("LOCKED: type `arm` first (valid 60 s). Make sure liquid is connected and a container is under the line.\n");
    return false;
}

static int drive_pin(const char *s)
{
    if (!s) return -1;
    if (s[0] == 'a' || s[0] == 'A') return GPIO_INA;
    if (s[0] == 'b' || s[0] == 'B') return GPIO_INB;
    return -1;
}

/* Sleep ms while sampling current; returns false if aborted by the current limit. */
static bool run_for_ms(int ms)
{
    int peak_mv = 0, peak_raw = 0, elapsed = 0;
    bool ok = true;
    while (elapsed < ms) {
        int step = ms - elapsed < 20 ? ms - elapsed : 20;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        elapsed += step;
        if (s_cur_gpio > 0) {
            int mv, raw = adc_read_gpio(s_cur_gpio, &mv);
            if (raw > peak_raw) peak_raw = raw;
            if (mv > peak_mv) peak_mv = mv;
            if (s_cur_limit_mv > 0 && mv > s_cur_limit_mv) { printf("ABORT: current-sense %d mV above limit %d mV\n", mv, s_cur_limit_mv); ok = false; break; }
        }
    }
    if (s_cur_gpio > 0) printf("current-sense GPIO%d peak: raw=%d mV=%d (idle baseline: run `adc` first)\n", s_cur_gpio, peak_raw, peak_mv);
    return ok;
}

/* ---------- commands ---------- */
static int cmd_levels(int argc, char **argv)
{
    printf("Input levels (internal pull-up on; pull-ups can mask a floating input):\n");
    for (size_t i = 0; i < N_IN; i++)
        printf("  %-30s %s   edges=%lu\n", INPUTS[i].name, gpio_get_level(INPUTS[i].gpio) ? "HIGH" : "LOW ", (unsigned long)s_edges[i]);
    return 0;
}

static int cmd_watch(int argc, char **argv)
{
    if (argc > 1) s_watch = strcmp(argv[1], "off") != 0;
    printf("watch %s\n", s_watch ? "on" : "off");
    return 0;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 3) { printf("usage: led <1|2|3|4|all> <on|off>\n"); return 1; }
    int on = strcmp(argv[2], "on") == 0;
    for (int i = 0; i < 4; i++)
        if (strcmp(argv[1], "all") == 0 || atoi(argv[1]) == i + 1) gpio_set_level(LED_GPIO[i], on);
    return 0;
}

static int cmd_ledwalk(int argc, char **argv)
{
    static const char *guess[4] = {"green", "blue", "amber", "red"};
    int ms = argc > 1 ? atoi(argv[1]) : 1500;
    for (int i = 0; i < 4; i++) {
        printf("LED%d (GPIO%d) ON now. Expected color: %s. Write down the color you see.\n", i + 1, LED_GPIO[i], guess[i]);
        gpio_set_level(LED_GPIO[i], 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(LED_GPIO[i], 0);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    printf("ledwalk done. If nothing lit, the LEDs may be active-low: try `led all off` then re-run and tell us what you saw.\n");
    return 0;
}

static int cmd_adc(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    int n = secs > 0 ? secs : 1;
    printf("ADC1 (11 dB range, ~0-2.5 V at the pin). GPIO8 is believed to be the battery divider. GPIO9/10 are motor pins and are skipped.\n");
    for (int k = 0; k < n; k++) {
        printf("t=%2ds ", k);
        for (int g = 1; g <= 8; g++) {
            int mv, raw = adc_read_gpio(g, &mv);
            printf(" GPIO%d=%d/%dmV", g, raw, mv);
        }
        printf("\n");
        if (k + 1 < n) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return 0;
}

static int cmd_curch(int argc, char **argv)
{
    if (argc < 2) { printf("current-sense GPIO=%d limit=%d mV\nusage: curch <1..8|off> [limit_mV]\n", s_cur_gpio, s_cur_limit_mv); return 0; }
    s_cur_gpio = strcmp(argv[1], "off") == 0 ? -1 : atoi(argv[1]);
    if (argc > 2) s_cur_limit_mv = atoi(argv[2]);
    printf("current-sense GPIO=%d limit=%d mV\n", s_cur_gpio, s_cur_limit_mv);
    return 0;
}

static int cmd_arm(int argc, char **argv)
{
    s_armed_until_us = esp_timer_get_time() + ARM_TIMEOUT_US;
    printf("ARMED for 60 s. pulse<=%d ms, hold/pwm<=%d ms. `stop` cancels.\n", PULSE_MAX_MS, HOLD_MAX_MS);
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    drive_off();
    s_armed_until_us = 0;
    printf("drive off, disarmed\n");
    return 0;
}

static int do_drive(int gpio, int ms, int max_ms, const char *what)
{
    if (!armed()) return 1;
    if (ms < 1 || ms > max_ms) { printf("%s limited to 1..%d ms\n", what, max_ms); return 1; }
    printf("%s GPIO%d for %d ms ...\n", what, gpio, ms);
    gpio_set_level(gpio == GPIO_INA ? GPIO_INB : GPIO_INA, 0);
    gpio_set_level(gpio, 1);
    run_for_ms(ms);
    gpio_set_level(gpio, 0);
    printf("done, both drive pins LOW\n");
    return 0;
}

static int cmd_pulse(int argc, char **argv)
{
    int p = argc > 1 ? drive_pin(argv[1]) : -1;
    if (p < 0 || argc < 3) { printf("usage: pulse <a|b> <ms>   (a=GPIO9 INA, b=GPIO10 INB; stock firmware pulses ~500 ms)\n"); return 1; }
    return do_drive(p, atoi(argv[2]), PULSE_MAX_MS, "pulse");
}

static int cmd_hold(int argc, char **argv)
{
    int p = argc > 1 ? drive_pin(argv[1]) : -1;
    if (p < 0 || argc < 3) { printf("usage: hold <a|b> <ms>   (max %d ms; only for a plain motor/pump, never dry)\n", HOLD_MAX_MS); return 1; }
    return do_drive(p, atoi(argv[2]), HOLD_MAX_MS, "hold");
}

static int cmd_pwm(int argc, char **argv)
{
    int p = argc > 1 ? drive_pin(argv[1]) : -1;
    if (p < 0 || argc < 4) { printf("usage: pwm <a|b> <duty%%> <ms>   (20 kHz, max %d ms)\n", HOLD_MAX_MS); return 1; }
    int duty = atoi(argv[2]), ms = atoi(argv[3]);
    if (!armed()) return 1;
    if (duty < 1 || duty > 100 || ms < 1 || ms > HOLD_MAX_MS) { printf("duty 1..100, ms 1..%d\n", HOLD_MAX_MS); return 1; }
    gpio_set_level(p == GPIO_INA ? GPIO_INB : GPIO_INA, 0);
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num = p, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = (1023 * duty) / 100 };
    ledc_channel_config(&c);
    printf("pwm GPIO%d duty %d%% for %d ms ...\n", p, duty, ms);
    run_for_ms(ms);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_level(p, 0);
    printf("done, both drive pins LOW\n");
    return 0;
}

static int cmd_report(int argc, char **argv)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    printf("\n===== drain_brain probe report (copy everything between the lines) =====\n");
    printf("probe %s, IDF %s, chip model %d rev %d, uptime %lld s\n", PROBE_VERSION, esp_get_idf_version(), ci.model, ci.revision, (long long)(esp_timer_get_time() / 1000000));
    printf("input             level  edges-since-boot\n");
    for (size_t i = 0; i < N_IN; i++)
        printf("%-30s %-5s %lu\n", INPUTS[i].name, gpio_get_level(INPUTS[i].gpio) ? "HIGH" : "LOW", (unsigned long)s_edges[i]);
    printf("ADC (raw/mV):");
    for (int g = 1; g <= 8; g++) { int mv, raw = adc_read_gpio(g, &mv); printf(" GPIO%d=%d/%d", g, raw, mv); }
    printf("\ncurrent-sense GPIO=%d limit=%d mV, ADC calibration %s\n", s_cur_gpio, s_cur_limit_mv, s_cali_ok ? "ok" : "unavailable");
    printf("===== end report =====\n");
    return 0;
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    esp_console_cmd_t c = { .command = name, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void app_main(void)
{
    io_setup();
    adc_setup();
    xTaskCreate(watch_task, "watch", 3072, NULL, 5, NULL);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "probe> ";
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));

    reg("levels", "print all input levels", cmd_levels);
    reg("watch", "watch on|off: print input changes as they happen", cmd_watch);
    reg("led", "led <1-4|all> <on|off>", cmd_led);
    reg("ledwalk", "ledwalk [ms]: light LED1..4 in turn", cmd_ledwalk);
    reg("adc", "adc [seconds]: read ADC1 GPIO1-8 (1 Hz if seconds given)", cmd_adc);
    reg("curch", "curch <1-8|off> [limit_mV]: current-sense pin to log/guard during drive", cmd_curch);
    reg("arm", "unlock drive commands for 60 s", cmd_arm);
    reg("stop", "drive pins low and disarm", cmd_stop);
    reg("pulse", "pulse <a|b> <ms<=600>", cmd_pulse);
    reg("hold", "hold <a|b> <ms<=3000>", cmd_hold);
    reg("pwm", "pwm <a|b> <duty%> <ms<=3000>", cmd_pwm);
    reg("report", "print a summary to paste into the results", cmd_report);

    printf("\ndrain_brain probe %s ready. Drive outputs are LOCKED. Type `help`.\n", PROBE_VERSION);
    printf("Inputs are being watched: press buttons, toggle bottle/float, unplug power; changes print below.\n");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
