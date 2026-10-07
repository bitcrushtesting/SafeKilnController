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
#include "kiln_core/fileslots.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal/hal_esp32s3.h"
#include "kiln_hmi/hmi.h"
#ifndef CONFIG_KILN_PLANT_SIM
#include "kiln_web/httpd.h"
#endif
#include "kiln_ports/port_system.h"

#ifdef CONFIG_KILN_PLANT_SIM
#include "kiln_sim/sim.h"
#include "driver/uart.h"
#endif

namespace {

const char *TAG = "kiln";

} // namespace

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

namespace {

kiln_app_t s_app;

/* Persistence is real even in the simulated build: NVS, the log partition and
 * the file store are flash, and QEMU emulates flash.  Only the plant is
 * simulated. */
kiln_port_flash_t    s_flash;
kiln_port_flash_t    s_fs_flash;
kiln_fileslots_t     s_fileslots;
kiln_port_kvstore_t  s_kv;
kiln_port_clock_t    s_clock;
kiln_port_system_t   s_system;
kiln_logring_t       s_ring;
kiln_port_logstore_t s_logstore;
kiln_port_filestore_t s_fs;

} // namespace

#ifdef CONFIG_KILN_PLANT_SIM
namespace {

kiln_sim_t             s_sim;
kiln_sim_ports_t       s_sim_ports;
kiln_sim_fs_t          s_sim_fs;      /* only if the flash store fails */

} // namespace
#else
namespace {

/* The real-hardware port instances.  Static, because the composition root owns
 * them for the lifetime of the image and kiln_app holds pointers to them. */
kiln_port_tc_t         s_tc_port;
kiln_port_tc_t         s_case_tc_port;
kiln_port_heat_t       s_heat_port;
kiln_port_current_t    s_current_port;
kiln_port_counters_t   s_counters_port;
kiln_port_door_t       s_door_port;
kiln_port_alarm_t      s_alarm_hw_port;
kiln_port_display_t    s_display_port;
kiln_port_input_t      s_input_port;
kiln_port_net_t        s_net_port;

} // namespace
#endif

/* --- port_alarm: a stub, until the buzzer adapter exists ---------------- */

namespace {

kiln_alarm_pattern_t s_alarm = KILN_ALARM_OFF;

void alarm_set(void *ctx, kiln_alarm_pattern_t pattern)
{
    (void)ctx;
    if (pattern != s_alarm) {
        s_alarm = pattern;
        ESP_LOGI(TAG, "alarm: %s",
                 pattern == KILN_ALARM_OFF      ? "off" :
                 pattern == KILN_ALARM_COMPLETE ? "complete" : "FAULT");
    }
}

const kiln_port_alarm_t s_alarm_port = { .ctx = NULL, .set = alarm_set };

/* --- the 10 ms output window (AD-07) ------------------------------------ */

/* Runs in esp_timer context: allocation-free, lock-free and short, which is
 * exactly what kiln_app_window_tick is written to be. */
void window_timer_cb(void *arg)
{
    (void)arg;
    kiln_app_window_tick(&s_app, WINDOW_PERIOD_MS);
}

/* --- tasks -------------------------------------------------------------- */

/* The safety task is the only writer of heat authority (AD-04) and the highest
 * priority task in the system (SR-13, NFR-03). */
void safety_task(void *arg)
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

void acquire_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(ACQUIRE_PERIOD_MS);

    for (;;) {
        kiln_app_acquire_cycle(&s_app, (float)ACQUIRE_PERIOD_MS / 1000.0f * TIME_ACCEL);
        vTaskDelayUntil(&next, period);
    }
}

void control_task(void *arg)
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
void logger_task(void *arg)
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

} // namespace

#ifdef CONFIG_KILN_PLANT_SIM

namespace {

/* The plant is "hardware", so it advances on its own clock rather than being
 * stepped by the firmware.  Core 0, so it cannot delay anything on core 1. */
void plant_task(void *arg)
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

void print_help(void)
{
    printf("\n"
           "  s  start the example program      a  abort\n"
           "  p  pause                          r  resume\n"
           "  c  clear the latched fault        m  manual 50%% duty\n"
           "  i  idle\n"
           "  Fault injection (toggle):\n"
           "   d  door switch open (SR-31)     D  no interlock fitted (warn 113)\n"
           "   1  relay fail-on (SR-25/SR-27)   2  relay fail-off (SR-26)\n"
           "   3  welded contactor (SR-27)      4  partial element loss (SR-28)\n"
           "   5  over-current (SR-29)          6  CT disconnected (FR-CUR-11)\n"
           "   7  SSR shorted (SR-08/SR-25)     8  thermocouple open (SR-04)\n"
           "   9  thermocouple stuck (SR-06)    0  lid open (SR-07)\n"
           "   x  clear all injections          h  this help\n\n");
}

void start_example(void)
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

void toggle_inject(uint32_t bit, const char *name)
{
    if (s_sim.inject & bit) {
        kiln_sim_clear(&s_sim, bit);
        ESP_LOGW(TAG, "injection cleared: %s", name);
    } else {
        kiln_sim_inject(&s_sim, bit);
        ESP_LOGW(TAG, "INJECTED: %s", name);
    }
}

void handle_key(int ch)
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
    case 'd': toggle_inject(KILN_INJ_DOOR_SWITCH_OPEN, "door switch open (SR-31)"); break;
    case 'D': toggle_inject(KILN_INJ_DOOR_ABSENT,      "no door interlock fitted (warning 113)"); break;
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

void report(void)
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

void console_task(void *arg)
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

} // namespace

#endif /* CONFIG_KILN_PLANT_SIM */

#ifndef CONFIG_KILN_PLANT_SIM
namespace {

/* --- the local interface (FR-HMI, architecture 5.4) --------------------
 *
 * On core 0 with the rest of the UI (AD-15), so a slow I2C frame cannot
 * contend with the control or safety tasks on core 1.  A whole 128x64 frame
 * is about 25 ms at 400 kHz, which is inside NFR-02's 50 ms on its own but
 * has no business sharing a core with the supervisor.
 *
 * The input is polled at 50 Hz because an encoder detent is a human gesture
 * and 20 ms of latency is imperceptible; the display is redrawn at 4 Hz,
 * comfortably inside FR-HMI-05's "twice per second and no more than 1 s
 * stale".  Redrawing only when the HMI says it is dirty keeps the bus quiet
 * when nothing is moving.
 */
kiln_hmi_t s_hmi;

void hmi_apply(const kiln_hmi_action_t *a)
{
    /* The HMI asks; kiln_app decides.  Every one of these can be refused --
     * SR-18 will not clear a live fault, FR-CUR-12 will not start a run
     * without current monitoring -- and a refusal is logged rather than
     * swallowed, because the operator pressed a button and deserves to know
     * it did nothing. */
    kiln_err_t e = KILN_OK;
    switch (a->kind) {
    case KILN_HMI_ACT_PAUSE:       e = kiln_app_pause(&s_app);       break;
    case KILN_HMI_ACT_RESUME:      e = kiln_app_resume(&s_app);      break;
    case KILN_HMI_ACT_ABORT:       e = kiln_app_abort(&s_app);       break;
    case KILN_HMI_ACT_ACK_SEGMENT: e = kiln_app_ack_segment(&s_app); break;
    case KILN_HMI_ACT_ACK_FAULT:
        e = kiln_app_clear_fault(&s_app);
        if (e == KILN_OK) {
            kiln_hal_heat_rearm();      /* SR-18: let the output arm again */
        }
        break;
    case KILN_HMI_ACT_START:
        /* No program store on hardware yet (tasklist E7), so there is nothing
         * to start.  The HMI already refuses to offer an empty list; this is
         * the belt to that braces. */
        e = KILN_ERR_NOT_FOUND;
        break;
    default: return;
    }
    if (e != KILN_OK) {
        ESP_LOGW(TAG, "hmi action %d refused: %s", (int)a->kind, kiln_err_str(e));
    }
}

void hmi_build_view(kiln_hmi_view_t *v)
{
    memset(v, 0, sizeof(*v));
    kiln_app_snapshot(&s_app, &v->snap);
    v->fault      = s_app.fault;
    v->warnings   = s_app.warnings;
    v->language   = (kiln_lang_t)s_app.cfg.language;
    v->fahrenheit = (s_app.cfg.units != 0u);
    v->elapsed_s  = (uint32_t)s_app.run_elapsed_s;
    v->power_w    = kiln_app_apparent_va(&s_app);
    v->energy_wh  = kiln_app_energy_wh(&s_app);
    v->kp = s_app.cfg.kp; v->ki = s_app.cfg.ki; v->kd = s_app.cfg.kd;

    kiln_fw_info_t fw = {};
    if (s_system.fw_info != NULL) {
        (void)s_system.fw_info(s_system.ctx, &fw);
    }
    (void)snprintf(v->version, sizeof(v->version), "%s", fw.version);
    v->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    /* FR-HMI-07: where the web interface is, which is the question an operator
     * standing at the kiln actually has. */
    kiln_net_status_t ns = {};
    if (s_net_port.status != nullptr &&
        s_net_port.status(s_net_port.ctx, &ns) == KILN_OK) {
        v->net_up = (ns.state == KILN_NET_STA_CONNECTED) ||
                    (ns.state == KILN_NET_AP_FALLBACK);
        /* The device's own name, not the SSID: FR-HMI-07 asks where the web
         * interface is, and "safekiln" is half that answer. */
        (void)snprintf(v->hostname, sizeof(v->hostname), "%s", ns.hostname);
        (void)snprintf(v->ip, sizeof(v->ip), "%s",
                       (ns.ip[0] != '\0') ? ns.ip : "no address");
    } else {
        v->net_up = false;
        (void)snprintf(v->hostname, sizeof(v->hostname), "%s", "wifi down");
        (void)snprintf(v->ip, sizeof(v->ip), "%s", "-");
    }
}

void hmi_task(void *arg)
{
    (void)arg;
    kiln_hmi_init(&s_hmi, s_app.cfg.dim_timeout_s);

    const TickType_t period = pdMS_TO_TICKS(20);     /* 50 Hz input poll */
    TickType_t       last   = xTaskGetTickCount();
    uint32_t         since_draw_ms = 0;

    for (;;) {
        const kiln_input_event_t ev =
            (s_input_port.poll != NULL) ? s_input_port.poll(s_input_port.ctx)
                                        : KILN_INPUT_NONE;

        kiln_hmi_view_t view;
        hmi_build_view(&view);

        const kiln_hmi_action_t a = kiln_hmi_update(&s_hmi, &view, ev, 20);
        if (a.kind != KILN_HMI_ACT_NONE) {
            hmi_apply(&a);
        }

        since_draw_ms += 20u;
        if (kiln_hmi_dirty(&s_hmi) || since_draw_ms >= 250u) {
            since_draw_ms = 0;
            if (s_display_port.present != NULL) {
                /* FR-HMI-14: a display that stops acknowledging is a warning,
                 * not a reason to stop controlling a kiln. */
                (void)s_display_port.present(s_display_port.ctx,
                                             kiln_hmi_frame(&s_hmi),
                                             KILN_DISPLAY_BYTES);
            }
            kiln_hmi_clear_dirty(&s_hmi);
        }
        vTaskDelayUntil(&last, period);
    }
}

} // namespace
#endif /* !CONFIG_KILN_PLANT_SIM */

extern "C" void app_main(void)
{
    /* NFR-15 / SR-14 / SR-15: the reset cause decides whether an interrupted
     * firing may be resumed at all, so it is read before anything else can
     * overwrite it. */
    kiln_hal_system_init(&s_system);
    const kiln_reset_cause_t cause = s_system.reset_cause(s_system.ctx);

    kiln_fw_info_t fw = {};
    (void)s_system.fw_info(s_system.ctx, &fw);
    ESP_LOGI(TAG, "Safe Kiln Controller %s (%s, IDF %s) on %s",
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

    /* Programs and run records (AD-10).  Raw flash on every build: QEMU
     * emulates flash, so the simulated firmware exercises the same store and
     * keeps its programs across a reboot rather than pretending to. */
    if (kiln_hal_flash_init(KILN_HAL_FS_PARTITION, &s_fs_flash) == KILN_OK) {
        const kiln_err_t fe = kiln_fileslots_mount(&s_fileslots, &s_fs_flash);
        if (fe == KILN_OK) {
            kiln_fileslots_bind(&s_fileslots, &s_fs);
            ports.filestore = &s_fs;
            ESP_LOGI(TAG, "file store: %u of %u regions used",
                     (unsigned)kiln_fileslots_used_regions(&s_fileslots),
                     (unsigned)s_fileslots.region_count);
        } else {
            /* As with the log: a kiln that cannot store programs is still a
             * kiln that can be watched, so this warns rather than halting. */
            ESP_LOGE(TAG, "file store unusable: %s", kiln_err_str(fe));
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
    ports.door     = &s_sim_ports.door;        /* SR-31 */

    /* Only if the flash-backed store above did not come up: a simulated kiln
     * with programs that vanish at reset is still more useful than none. */
    if (ports.filestore == nullptr) {
        kiln_sim_fs_init(&s_sim_fs);
        kiln_sim_fs_bind(&s_sim_fs, &s_fs);
        ports.filestore = &s_fs;
        ESP_LOGW(TAG, "file store in RAM: programs will not survive a reset");
    }

    ESP_LOGW(TAG, "SIMULATED PLANT: no hardware output is driven. "
                  "time acceleration %gx", (double)TIME_ACCEL);
#else
    /* --- real hardware (M2 / M4b) --------------------------------------
     *
     * The composition root, and the only place that binds a port to a concrete
     * adapter (architecture 5.3).  Pins come from board_pins.h and appear
     * nowhere else (HR-10).
     *
     * Order matters once: the heat adapter puts every output in its safe state,
     * so it is bound before anything can ask for heat (SR-21, NFR-09). */
    kiln_hal_heat_init(&s_heat_port);
    ports.heat = &s_heat_port;

    /* Two MAX31856 on one bus with separate chip selects (HR-02).  The chamber
     * is required; the enclosure is optional, and SR-11 stands down without it
     * rather than refusing to run. */
    if (kiln_hal_tc_init(0, &s_tc_port) == KILN_OK) {
        ports.tc = &s_tc_port;
    } else {
        ESP_LOGE(TAG, "chamber thermocouple front end did not answer");
    }
    if (kiln_hal_tc_init(1, &s_case_tc_port) == KILN_OK) {
        ports.case_tc = &s_case_tc_port;
    } else {
        ESP_LOGW(TAG, "no enclosure front end: SR-11 stands down");
    }

    /* FR-CUR-12 refuses to start a run when this is unavailable unless
     * monitoring has been explicitly disabled, so a failure here is reported
     * loudly rather than folded into a warning. */
    if (kiln_hal_current_init(&s_current_port) == KILN_OK) {
        ports.current = &s_current_port;
    } else {
        ESP_LOGE(TAG, "current front end unavailable: runs will be refused "
                      "unless current monitoring is disabled (FR-CUR-12)");
    }

    if (kiln_hal_counters_init(&s_counters_port) == KILN_OK) {
        ports.counters = &s_counters_port;
    }

    /* SR-31.  Whether a switch is actually fitted is an installation fact the
     * board cannot read, so it is told: true here, and tasklist G4 moves it to
     * a configuration item.  Passing true on a kiln with no switch fitted and
     * J9 left open means the coil never closes, which is safe and extremely
     * visible; passing false suppresses the rule and raises warning 113. */
    kiln_hal_door_init(&s_door_port, true);
    ports.door = &s_door_port;

    kiln_hal_alarm_init(&s_alarm_hw_port);
    ports.alarm = &s_alarm_hw_port;

    /* HR-04 and HR-05.  The display is bound even when the panel did not
     * answer: FR-HMI-14 keeps the kiln running without one, and available()
     * is how warning 104 gets raised rather than inferred. */
    (void)kiln_hal_display_init(&s_display_port);
    (void)kiln_hal_input_init(&s_input_port);

    ESP_LOGW(TAG, "REAL HARDWARE: outputs are live. Fit the independent "
                  "over-temperature cutout (HR-13) before firing.");
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

#ifndef CONFIG_KILN_PLANT_SIM
    /* FR-NET-01..FR-NET-09, started after the application and never waited on.
     * FR-NET-07 requires that losing the network cannot alter a running
     * firing, so the kiln is already fully operational before the radio is
     * touched and nothing above depends on this succeeding.
     *
     * It does not go into kiln_app_ports_t, because the application has no use
     * for it: the network is read by the HMI's network screen (FR-HMI-07) and
     * by the web API, neither of which is a control path. */
    if (kiln_hal_net_init(&s_app.cfg, &s_net_port) != KILN_OK) {
        ESP_LOGE(TAG, "WiFi did not start; the kiln runs, the web does not");
    }

    /* The REST API over HTTP (FR-WEB-19 to FR-WEB-26).  Read-only, so there is
     * nothing to authenticate and no way for a client to reach the kiln.
     * Started last and never waited on, for FR-NET-07's reason: the firing
     * must not depend on any of this. */
    static kiln_api_ctx_t s_api;
    s_api.app       = &s_app;
    s_api.filestore = ports.filestore;
    s_api.logstore  = ports.logstore;
    s_api.system    = &s_system;
    s_api.net       = &s_net_port;
    s_api.ring      = &s_ring;
    if (kiln_httpd_start(&s_api) != KILN_OK) {
        ESP_LOGE(TAG, "the web server did not start; the kiln is unaffected");
    }
#endif

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

#ifndef CONFIG_KILN_PLANT_SIM
    /* AD-15: the UI lives on core 0, where it cannot contend with control or
     * safety.  Low priority: FR-HMI-14 says the display is the least important
     * thing in the box. */
    xTaskCreatePinnedToCore(hmi_task, "hmi", 4096, NULL, 4, NULL, 0);
#endif

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
