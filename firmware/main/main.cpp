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
#include "kiln_app/run_index.h"
#include "kiln_core/faults.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal/hal_esp32s3.h"
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

/* Persistence is real even in the simulated build: NVS and the log partition are
 * flash, and QEMU emulates flash.  Only the plant is simulated. */
static kiln_port_flash_t    s_flash;
static kiln_port_kvstore_t  s_kv;
static kiln_port_clock_t    s_clock;
static kiln_port_system_t   s_system;
static kiln_logring_t       s_ring;
static kiln_port_logstore_t s_logstore;

#ifdef CONFIG_KILN_PLANT_SIM
static kiln_sim_t             s_sim;
static kiln_sim_ports_t       s_sim_ports;
static kiln_sim_fs_t          s_sim_fs;      /* until the LittleFS adapter exists */
static kiln_port_filestore_t  s_fs;
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

/* The only task that touches flash (FR-LOG-14, architecture 6.1).  Core 0, so an
 * erase -- tens of milliseconds -- cannot delay control or safety on core 1. */
static void logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* A bounded batch, so a backlog is worked off without monopolising the
         * flash for a whole queue's worth of erases. */
        if (kiln_app_log_drain(&s_app, 8) == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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
    case 'l': {
        /* FR-LOG-13 is an operator action with a warning attached; here it is
         * just the quickest way to see the ring start over. */
        ESP_LOGW(TAG, "erasing the sample log: %s",
                 kiln_err_str(kiln_logring_erase_all(&s_ring)));
        break;
    }
    case 'R': {
        const uint8_t n = kiln_run_index_count(&s_fs);
        printf("\n  %u stored run record(s)\n", n);
        for (uint8_t i = 0; i < KILN_RUN_SLOTS; i++) {
            kiln_run_record_t r;
            if (kiln_run_index_get_slot(&s_fs, i, &r) != KILN_OK) continue;
            printf("    run %-4u %-28s peak %6.1f degC  %6u s  %s%s\n",
                   (unsigned)r.run_id, r.program.name, (double)r.peak_c,
                   (unsigned)r.duration_s, kiln_run_end_str((kiln_run_end_t)r.end_reason),
                   (r.flags & KILN_RUN_FLAG_TRUNCATED) ? "  [samples overwritten]" : "");
        }
        printf("\n");
        break;
    }
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

    kiln_logstore_stats_t ls;
    if (kiln_logring_stats(&s_ring, &ls) == KILN_OK) {
        printf("    log: %u records, %u sectors erased, %u dropped, %u errors%s\n",
               (unsigned)ls.records_stored, (unsigned)ls.erase_count,
               (unsigned)s_app.log_dropped, (unsigned)s_app.log_errors,
               ls.available ? "" : "  [UNAVAILABLE]");
    }

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
    /* Value-initialised rather than fully designated: uart_config_t is an
      * ESP-IDF struct, and naming every member here would turn an IDF upgrade
      * that adds one into a build break. */
    uart_config_t uc = {};
    uc.baud_rate  = 115200;
    uc.data_bits  = UART_DATA_8_BITS;
    uc.parity     = UART_PARITY_DISABLE;
    uc.stop_bits  = UART_STOP_BITS_1;
    uc.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
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

extern "C" void app_main(void)
{
    /* NFR-15 / SR-14 / SR-15: the reset cause decides whether an interrupted
     * firing may be resumed at all, so it is read before anything else can
     * overwrite it. */
    kiln_hal_system_init(&s_system);
    const kiln_reset_cause_t cause = s_system.reset_cause(s_system.ctx);

    kiln_fw_info_t fw = {};
    (void)s_system.fw_info(s_system.ctx, &fw);
    ESP_LOGI(TAG, "KilnControl %s (%s, IDF %s) on %s",
             fw.version, fw.build_time, fw.idf_version, fw.target);
    ESP_LOGI(TAG, "reset cause %d%s", (int)cause,
             kiln_reset_was_abnormal(cause) ? "  (ABNORMAL -- SR-14)" : "");

    kiln_app_ports_t ports = {};
    ports.alarm = &s_alarm_port;

    /* --- storage, which is real flash even here --------------------------- */

    kiln_hal_clock_init(&s_clock);
    ports.clock = &s_clock;

    if (kiln_hal_kvstore_init(&s_kv) == KILN_OK) {
        ports.kvstore = &s_kv;
    } else {
        /* FR-CFG-05: defaults, and a warning.  A kiln with default limits is
         * safer than a kiln that refuses to boot. */
        ESP_LOGE(TAG, "no NVS: configuration will not persist");
    }

    if (kiln_hal_flash_init(KILN_HAL_LOG_PARTITION, &s_flash) == KILN_OK) {
        const kiln_err_t me = kiln_logring_mount(&s_ring, &s_flash);
        if (me == KILN_OK || me == KILN_ERR_CORRUPT) {
            /* CORRUPT here means "no sector header anywhere", which is what a
             * blank partition looks like -- not a problem, just an empty log. */
            kiln_logring_bind(&s_ring, &s_logstore);
            ports.logstore = &s_logstore;

            kiln_logstore_stats_t st;
            if (kiln_logring_stats(&s_ring, &st) == KILN_OK) {
                ESP_LOGI(TAG, "log: %u records stored of %u (%u h at %u s), "
                              "%u sectors",
                         (unsigned)st.records_stored, (unsigned)st.records_total,
                         (unsigned)(st.records_total * 10u / 3600u), 10u,
                         (unsigned)st.sectors_total);
            }
        } else {
            /* FR-LOG-14: carry on without a log, and warn. */
            ESP_LOGE(TAG, "log partition unreadable: %s", kiln_err_str(me));
        }
    }

#ifdef CONFIG_KILN_PLANT_SIM
    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);

    /* The charge pump is hardware and does not accelerate.  Everything the
     * simulator integrates is in accelerated seconds, so a decay expressed in
     * real seconds would expire between two safety refreshes 100 ms apart and
     * drop the contactor continuously -- which then reads, correctly, as no
     * heater current and latches SR-26 within seconds of starting a firing.
     *
     * Scaling it keeps the circuit's *real* 1 s against the real refresh rate.
     * The same reasoning applies to anything else in the simulator whose time
     * constant belongs to the hardware rather than to the kiln. */
    sc.enable_decay_s = 1.0f * TIME_ACCEL;

    kiln_sim_init(&s_sim, &sc);
    kiln_sim_bind(&s_sim, &s_sim_ports);

    ports.tc       = &s_sim_ports.tc;
    ports.case_tc  = &s_sim_ports.case_tc;
    ports.heat     = &s_sim_ports.heat;
    ports.current  = &s_sim_ports.current;
    ports.counters = &s_sim_ports.counters;

    /* Programs and run records.  RAM, until LittleFS is vendored -- see the note
     * on kiln_sim_fs_t. */
    kiln_sim_fs_init(&s_sim_fs);
    kiln_sim_fs_bind(&s_sim_fs, &s_fs);
    ports.filestore = &s_fs;

    ESP_LOGW(TAG, "SIMULATED PLANT: no hardware output is driven. "
                  "time acceleration %gx", (double)TIME_ACCEL);
#else
#error "No hardware adapters yet: build with CONFIG_KILN_PLANT_SIM (see sdkconfig.qemu)."
#endif

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    /* Gains for the *default simulated plant* -- 1500 degC of authority, a 40 min
     * time constant and 90 s of transport lag -- derived the way you would for
     * any FOPDT process and then checked against it: worst tracking error 5.7
     * degC through the cone 6 example, against the 23.9 degC and a tripped SR-08
     * that a four-times-higher proportional gain produced.
     *
     * They are not gains for a real kiln.  FR-TUN-11's warning 108 stands until
     * one has actually been tuned, which is what FR-TUN exists for. */
    cfg.kp = 1.5f;
    cfg.ki = 0.006f;
    cfg.kd = 90.0f;

    const kiln_err_t e = kiln_app_init(&s_app, &ports, &cfg);
    if (e != KILN_OK) {
        ESP_LOGE(TAG, "kiln_app_init failed: %s", kiln_err_str(e));
        return;
    }

    /* Configuration, seeded programs, the run-id sequence, SR-17's latched fault
     * and FR-RUN-08's recovery decision.  The outage length is unknown here: a
     * device with no RTC battery cannot tell how long it was off, and FR-RUN-08
     * treats an unknown outage as too long, which is the conservative reading. */
    const kiln_err_t be = kiln_app_boot(&s_app, cause, -1.0f);
    if (be != KILN_OK) {
        ESP_LOGE(TAG, "configuration storage failed; defaults are in use");
    }

    const kiln_recovery_decision_t *rec = kiln_app_recovery(&s_app);
    if (rec->action != KILN_RECOVER_NO_RUN) {
        ESP_LOGW(TAG, "interrupted firing: %s", rec->reason ? rec->reason : "");
    }
    if (s_app.fault != KILN_FAULT_NONE) {
        ESP_LOGE(TAG, "FAULT %u %s latched: %s", (unsigned)s_app.fault,
                 kiln_fault_label(s_app.fault), kiln_fault_cause(s_app.fault));
    }

    /* FR-LOG-09: the ring only goes back so far, and a run whose samples have
     * gone is marked so a chart with no data in it is not a mystery. */
    if (ports.filestore && ports.logstore) {
        uint8_t oldest[KILN_LOG_RECORD_BYTES];
        if (kiln_logring_last_record(&s_ring, 0, oldest) == KILN_OK) {
            /* The run id of the oldest *sector*, which is what the ring knows. */
            (void)kiln_run_index_mark_truncated(ports.filestore, s_ring.run_id);
        }
    }

#ifdef CONFIG_KILN_PLANT_SIM
    xTaskCreatePinnedToCore(plant_task, "plant", 3072, NULL, 10, NULL, 0);
#endif

    /* Core 0 and low priority: the one task allowed to block on flash. */
    xTaskCreatePinnedToCore(logger_task, "logger", 3072, NULL, 8, NULL, 0);

    /* AD-15: control and safety on core 1, where the WiFi stack cannot reach
     * them.  Priorities from the table in architecture section 6.1. */
    xTaskCreatePinnedToCore(safety_task,  "safety",  3072, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(acquire_task, "acquire", 3072, NULL, 19, NULL, 1);
    xTaskCreatePinnedToCore(control_task, "control", 4096, NULL, 18, NULL, 1);

    /* As uc above.  This one additionally had its designators out of
      * declaration order, which C99 permitted and C++20 does not. */
    esp_timer_create_args_t window_args = {};
    window_args.callback        = window_timer_cb;
    window_args.dispatch_method = ESP_TIMER_TASK;
    window_args.name            = "heat_window";
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
