/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Composition root -- architecture section 5.3.  The only file in the firmware
 * that binds a port to a concrete adapter, and the only one that creates a task.
 *
 * With CONFIG_KILN_PLANT_SIM the plant is kiln_sim and the firmware runs under
 * QEMU: see sdkconfig.qemu for why substituting at the port boundary is the only
 * thing that can work there, and what it does and does not prove.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

#include "kiln_app/app.h"
#include "kiln_core/faults.h"
#include "kiln_core/profile.h"
#include "kiln_ports/port_system.h"

#ifdef CONFIG_KILN_PLANT_SIM
#include "kiln_sim/sim.h"
#include "driver/uart.h"
#endif

static const char *TAG = "kiln";

/* Periods, from the task table of architecture section 6.1. */
#define WINDOW_PERIOD_MS   10
#define SAFETY_PERIOD_MS  100
#define ACQUIRE_PERIOD_MS 250
#define CONTROL_PERIOD_MS 1000

#ifdef CONFIG_KILN_SIM_TIME_ACCEL
#define TIME_ACCEL ((float)CONFIG_KILN_SIM_TIME_ACCEL)
#else
#define TIME_ACCEL 1.0f
#endif

static kiln_app_t s_app;

#ifdef CONFIG_KILN_PLANT_SIM
static kiln_sim_t       s_sim;
static kiln_sim_ports_t s_sim_ports;
#endif

/* --- port_alarm: a stub, until the buzzer adapter exists ---------------- */

static kiln_alarm_pattern_t s_alarm = KILN_ALARM_OFF;

static void alarm_set(void *ctx, kiln_alarm_pattern_t pattern)
{
    (void)ctx;
    if (pattern != s_alarm) {
        s_alarm = pattern;
        ESP_LOGI(TAG, "alarm: %s",
                 pattern == KILN_ALARM_OFF      ? "off" :
                 pattern == KILN_ALARM_COMPLETE ? "complete" : "FAULT");
    }
}

static const kiln_port_alarm_t s_alarm_port = { .ctx = NULL, .set = alarm_set };

/* --- the 10 ms output window (AD-07) ------------------------------------ */

/* Runs in esp_timer context: allocation-free, lock-free and short, which is
 * exactly what kiln_app_window_tick is written to be. */
static void window_timer_cb(void *arg)
{
    (void)arg;
    kiln_app_window_tick(&s_app, WINDOW_PERIOD_MS);
}

/* --- tasks -------------------------------------------------------------- */

/* The safety task is the only writer of heat authority (AD-04) and the highest
 * priority task in the system (SR-13, NFR-03). */
static void safety_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(SAFETY_PERIOD_MS);

    esp_task_wdt_add(NULL);     /* SR-14 */

    for (;;) {
        kiln_app_safety_cycle(&s_app, (float)SAFETY_PERIOD_MS / 1000.0f * TIME_ACCEL);
        esp_task_wdt_reset();
        vTaskDelayUntil(&next, period);
    }
}

static void acquire_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(ACQUIRE_PERIOD_MS);

    for (;;) {
        kiln_app_acquire_cycle(&s_app, (float)ACQUIRE_PERIOD_MS / 1000.0f * TIME_ACCEL);
        vTaskDelayUntil(&next, period);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);

    esp_task_wdt_add(NULL);     /* SR-14 */

    for (;;) {
        kiln_app_control_cycle(&s_app, (float)CONTROL_PERIOD_MS / 1000.0f * TIME_ACCEL);
        esp_task_wdt_reset();
        vTaskDelayUntil(&next, period);
    }
}

#ifdef CONFIG_KILN_PLANT_SIM

/* The plant is "hardware", so it advances on its own clock rather than being
 * stepped by the firmware.  Core 0, so it cannot delay anything on core 1. */
static void plant_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(WINDOW_PERIOD_MS);

    for (;;) {
        kiln_sim_step(&s_sim, (float)WINDOW_PERIOD_MS / 1000.0f * TIME_ACCEL);
        vTaskDelayUntil(&next, period);
    }
}

/* --- console: watch the firing, and inject the faults ------------------- */

static void print_help(void)
{
    printf("\n"
           "  s  start the example program      a  abort\n"
           "  p  pause                          r  resume\n"
           "  c  clear the latched fault        m  manual 50%% duty\n"
           "  i  idle\n"
           "  Fault injection (toggle):\n"
           "   1  relay fail-on (SR-25/SR-27)   2  relay fail-off (SR-26)\n"
           "   3  welded contactor (SR-27)      4  partial element loss (SR-28)\n"
           "   5  over-current (SR-29)          6  CT disconnected (FR-CUR-11)\n"
           "   7  SSR shorted (SR-08/SR-25)     8  thermocouple open (SR-04)\n"
           "   9  thermocouple stuck (SR-06)    0  lid open (SR-07)\n"
           "   x  clear all injections          h  this help\n\n");
}

static void start_example(void)
{
    kiln_program_t prog;
    const uint8_t which = (uint8_t)CONFIG_KILN_SIM_AUTOSTART_PROGRAM;

    if (kiln_profile_example(which, &prog) != KILN_OK) {
        ESP_LOGE(TAG, "no such example program: %u", which);
        return;
    }
    const kiln_err_t e = kiln_app_start(&s_app, &prog);
    if (e == KILN_OK) {
        ESP_LOGI(TAG, "started \"%s\": %u segments, peak %.0f degC",
                 prog.name, prog.segment_count, (double)kiln_profile_peak_c(&prog));
    } else {
        ESP_LOGW(TAG, "start refused: %s", kiln_err_str(e));
    }
}

static void toggle_inject(uint32_t bit, const char *name)
{
    if (s_sim.inject & bit) {
        kiln_sim_clear(&s_sim, bit);
        ESP_LOGW(TAG, "injection cleared: %s", name);
    } else {
        kiln_sim_inject(&s_sim, bit);
        ESP_LOGW(TAG, "INJECTED: %s", name);
    }
}

static void handle_key(int ch)
{
    switch (ch) {
    case 's': start_example(); break;
    case 'a': ESP_LOGI(TAG, "abort: %s", kiln_err_str(kiln_app_abort(&s_app))); break;
    case 'p': ESP_LOGI(TAG, "pause: %s", kiln_err_str(kiln_app_pause(&s_app))); break;
    case 'r': ESP_LOGI(TAG, "resume: %s", kiln_err_str(kiln_app_resume(&s_app))); break;
    case 'c': ESP_LOGI(TAG, "clear: %s", kiln_err_str(kiln_app_clear_fault(&s_app))); break;
    case 'm': ESP_LOGI(TAG, "manual: %s", kiln_err_str(kiln_app_manual(&s_app, 500))); break;
    case 'i': ESP_LOGI(TAG, "idle: %s", kiln_err_str(kiln_app_idle(&s_app))); break;

    case '1': toggle_inject(KILN_INJ_RELAY_FAIL_ON,   "relay fail-on (SR-25)"); break;
    case '2': toggle_inject(KILN_INJ_RELAY_FAIL_OFF,  "relay fail-off (SR-26)"); break;
    case '3': toggle_inject(KILN_INJ_CONTACTOR_WELD,  "welded contactor (SR-27)"); break;
    case '4': toggle_inject(KILN_INJ_ELEMENT_PARTIAL, "partial element loss (SR-28)"); break;
    case '5': toggle_inject(KILN_INJ_OVERCURRENT,     "over-current (SR-29)"); break;
    case '6': toggle_inject(KILN_INJ_CT_DISCONNECTED, "CT disconnected (FR-CUR-11)"); break;
    case '7': toggle_inject(KILN_INJ_SSR_SHORTED,     "SSR shorted (SR-08/SR-25)"); break;
    case '8': toggle_inject(KILN_INJ_TC_OPEN,         "thermocouple open (SR-04)"); break;
    case '9': toggle_inject(KILN_INJ_TC_STUCK,        "thermocouple stuck (SR-06)"); break;
    case '0': toggle_inject(KILN_INJ_LID_OPEN,        "lid open (SR-07)"); break;
    case 'x': kiln_sim_clear(&s_sim, 0xFFFFFFFFu);
              ESP_LOGW(TAG, "all injections cleared"); break;
    case 'h': print_help(); break;
    default: break;
    }
}

static void report(void)
{
    kiln_snapshot_t s;
    kiln_app_snapshot(&s_app, &s);

    char flags[8];
    size_t n = 0;
    if (s.current_flags & KILN_CURF_CONDUCTION) flags[n++] = 'C';
    if (s.current_flags & KILN_CURF_LEAKAGE)    flags[n++] = 'L';
    if (s.current_flags & KILN_CURF_SKIPPED)    flags[n++] = 'S';
    if (s.current_flags & KILN_CURF_STALE)      flags[n++] = '~';
    if (s.current_flags & KILN_CURF_CT_FAULT)   flags[n++] = '!';
    flags[n] = '\0';

    printf("[%-6s] %7.1f degC  sp %7.1f  rate %+7.1f degC/h  duty %4u  "
           "I %5.2f A %-5s ref %5.2f  seg %u/%u%s%s%s\n",
           kiln_state_label((kiln_state_t)s.state),
           (double)s.kiln_c, (double)s.setpoint_c, (double)s.rate_c_per_h,
           s.duty_permille, (double)s.current_a, flags, (double)s.current_ref_a,
           (unsigned)(s.segment_index + 1u), s.segment_count,
           s.heat_authorised ? "  HEAT" : "",
           s.holdback_active ? "  HOLDBACK" : "",
           kiln_sim_contactor(&s_sim) ? "  [contactor closed]" : "");

    if (s_app.fault != KILN_FAULT_NONE) {
        printf("    FAULT %u %s (%s): %s\n",
               (unsigned)s_app.fault, kiln_fault_label(s_app.fault),
               kiln_fault_requirement(s_app.fault), kiln_fault_cause(s_app.fault));
    }
    for (uint8_t b = 0; b < KILN_WARN_COUNT; b++) {
        if (s_app.warnings & KILN_WARN_BIT(b)) {
            printf("    warning %u %s: %s\n", kiln_warn_code((kiln_warn_bit_t)b),
                   kiln_warn_label((kiln_warn_bit_t)b),
                   kiln_warn_cause((kiln_warn_bit_t)b));
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;

    /* The UART driver, so a keypress can be polled without blocking the task. */
    const uart_config_t uc = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    bool have_uart = (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) == ESP_OK) &&
                     (uart_param_config(UART_NUM_0, &uc) == ESP_OK);

    print_help();

    uint32_t ticks = 0;
    for (;;) {
        if (have_uart) {
            uint8_t ch = 0;
            while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) handle_key((int)ch);
        }
        if (++ticks % 10u == 0u) report();     /* once a second */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#endif /* CONFIG_KILN_PLANT_SIM */

/* --- reset cause (NFR-15, SR-14, SR-15) -------------------------------- */

static kiln_reset_cause_t map_reset_cause(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return KILN_RESET_POWER_ON;
    case ESP_RST_SW:       return KILN_RESET_SOFTWARE;
    case ESP_RST_PANIC:    return KILN_RESET_PANIC;
    case ESP_RST_TASK_WDT: return KILN_RESET_TASK_WDT;
    case ESP_RST_INT_WDT:  return KILN_RESET_INT_WDT;
    case ESP_RST_WDT:      return KILN_RESET_RTC_WDT;
    case ESP_RST_BROWNOUT: return KILN_RESET_BROWNOUT;
    case ESP_RST_DEEPSLEEP:return KILN_RESET_DEEPSLEEP;
    case ESP_RST_EXT:      return KILN_RESET_EXTERNAL;
    default:               return KILN_RESET_UNKNOWN;
    }
}

void app_main(void)
{
    const kiln_reset_cause_t cause = map_reset_cause();

    ESP_LOGI(TAG, "KilnControl starting; reset cause %d%s", (int)cause,
             kiln_reset_was_abnormal(cause) ? " (ABNORMAL)" : "");

    kiln_app_ports_t ports = {0};
    ports.alarm = &s_alarm_port;

#ifdef CONFIG_KILN_PLANT_SIM
    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    kiln_sim_init(&s_sim, &sc);
    kiln_sim_bind(&s_sim, &s_sim_ports);

    ports.tc       = &s_sim_ports.tc;
    ports.case_tc  = &s_sim_ports.case_tc;
    ports.heat     = &s_sim_ports.heat;
    ports.current  = &s_sim_ports.current;
    ports.counters = &s_sim_ports.counters;

    ESP_LOGW(TAG, "SIMULATED PLANT: no hardware output is driven. "
                  "time acceleration %gx", (double)TIME_ACCEL);
#else
#error "No hardware adapters yet: build with CONFIG_KILN_PLANT_SIM (see sdkconfig.qemu)."
#endif

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    /* Gains that are at least in the right order of magnitude for the default
     * simulated plant, so a firing tracks rather than oscillates.  FR-TUN-11's
     * warning 108 stands until this kiln has actually been tuned. */
    cfg.kp = 8.0f;
    cfg.ki = 0.004f;
    cfg.kd = 120.0f;

    const kiln_err_t e = kiln_app_init(&s_app, &ports, &cfg);
    if (e != KILN_OK) {
        ESP_LOGE(TAG, "kiln_app_init failed: %s", kiln_err_str(e));
        return;
    }

#ifdef CONFIG_KILN_PLANT_SIM
    xTaskCreatePinnedToCore(plant_task, "plant", 3072, NULL, 10, NULL, 0);
#endif

    /* AD-15: control and safety on core 1, where the WiFi stack cannot reach
     * them.  Priorities from the table in architecture section 6.1. */
    xTaskCreatePinnedToCore(safety_task,  "safety",  3072, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(acquire_task, "acquire", 3072, NULL, 19, NULL, 1);
    xTaskCreatePinnedToCore(control_task, "control", 4096, NULL, 18, NULL, 1);

    const esp_timer_create_args_t window_args = {
        .callback = window_timer_cb,
        .name     = "heat_window",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t window_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&window_args, &window_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(window_timer, WINDOW_PERIOD_MS * 1000));

#ifdef CONFIG_KILN_PLANT_SIM
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 5, NULL, 0);

#ifdef CONFIG_KILN_SIM_AUTOSTART
    /* Let one acquisition cycle land first: FR-RUN-02's self-check refuses to
     * start a run on a channel that has not answered yet. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    start_example();
#endif
#endif
}
