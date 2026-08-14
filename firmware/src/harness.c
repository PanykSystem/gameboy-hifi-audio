#include "harness.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_sm.h"
#include "pinmap.h"

static const char *TAG = "harness";

// Battery rail present. Measured 284 mV bare on the programming header; an
// installed board runs 2000..3200 mV (the firmware boots down to 2.0 V). The
// threshold sits in the empty middle, ~3.5x above the bare reading and 2x below
// the lowest real one.
#define VBAT_PRESENT_MV   1000

// VOL wiper floor lives in app_sm.h (APP_SM_VOL_OPEN_RAW) so the wheel poll and
// this report agree on one number.
#define VOL_OPEN_RAW      APP_SM_VOL_OPEN_RAW

// How long PAIR must sit LOW before it is a stuck line rather than a press. The
// longest real hold is the factory-reset countdown at CONFIG_GBHIFI_FACTORY_RESET_S
// (10 s); the hold menu tops out at 9 s. 20 s clears both.
#define PAIR_STUCK_MS     20000

#define SAMPLE_PERIOD_MS  1000

// Milliseconds since boot at which PAIR was first seen LOW in the current
// continuous run, or -1 when it is currently HIGH. Maintained by sample_task.
static int64_t s_pair_low_since_ms = -1;
static bool    s_sampler_running   = false;
static harness_change_cb_t s_change_cb = NULL;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Continuous-LOW dwell for PAIR in milliseconds, or -1 if it is HIGH or the
// sampler has not run yet.
static int64_t pair_low_ms(void)
{
    if (!s_sampler_running || s_pair_low_since_ms < 0) return -1;
    return now_ms() - s_pair_low_since_ms;
}

void harness_set_change_cb(harness_change_cb_t cb)
{
    s_change_cb = cb;
}

static void sample_task(void *arg)
{
    // Debounced change detection: a new state is published only once TWO
    // consecutive samples agree on it. A single odd sample -- a transient ADC
    // read failure, a glitch caught mid-edge -- used to flip up to three states
    // at once for exactly one second (VBAT "unavailable" also flips installed,
    // which reclassifies HP), and every flip notified the web diag card: the
    // "flashing warnings". Real conditions persist and still get through, one
    // sample period later. `published` is what the subscriber last saw; `prev`
    // is the previous evaluation. Both seeded out of range so the first stable
    // report publishes.
    harness_state_t published[HARNESS_SIG_COUNT];
    harness_state_t prev[HARNESS_SIG_COUNT];
    for (int i = 0; i < HARNESS_SIG_COUNT; i++) {
        published[i] = (harness_state_t)0xFF;
        prev[i]      = (harness_state_t)0xFF;
    }

    for (;;) {
        bool low = (gpio_get_level(PIN_CP_BUTTON) == 0);
        if (low) {
            if (s_pair_low_since_ms < 0) s_pair_low_since_ms = now_ms();
        } else {
            s_pair_low_since_ms = -1;
        }

        // Only the states gate a notification, not the raw values: VBAT and the
        // wheel move every sample, and pushing on each would be a 1 Hz spam of
        // an otherwise-idle link.
        if (s_change_cb) {
            harness_report_t rep;
            harness_eval(&rep);
            bool changed = false;
            for (int i = 0; i < HARNESS_SIG_COUNT; i++) {
                harness_state_t cur = rep.sig[i].state;
                if (cur == prev[i] && cur != published[i]) {
                    published[i] = cur;
                    changed = true;
                }
                prev[i] = cur;
            }
            if (changed) s_change_cb(&rep);
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t harness_init(void)
{
    // Seed the dwell from the live level so a pin already LOW at init counts
    // from now rather than from the first tick.
    s_pair_low_since_ms = (gpio_get_level(PIN_CP_BUTTON) == 0) ? now_ms() : -1;
    s_sampler_running = true;
    xTaskCreate(sample_task, "harness", 2048, NULL, 2, NULL);
    return ESP_OK;
}

bool harness_rail_present(void)
{
    int mv = app_sm_read_vbat_mv();
    return (mv >= 0) && (mv >= VBAT_PRESENT_MV);
}

bool harness_vol_open(void)
{
    return app_sm_vol_line_open();
}

void harness_eval(harness_report_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // VBAT. Drives the overall verdict, so it is evaluated first.
    int vbat_mv = app_sm_read_vbat_mv();
    out->sig[HARNESS_SIG_VBAT].value = vbat_mv;
    if (vbat_mv < 0) {
        out->sig[HARNESS_SIG_VBAT].state = HARNESS_UNKNOWN;
    } else {
        out->sig[HARNESS_SIG_VBAT].state =
            (vbat_mv >= VBAT_PRESENT_MV) ? HARNESS_OK : HARNESS_OPEN;
    }
    out->installed = (out->sig[HARNESS_SIG_VBAT].state == HARNESS_OK);

    // VOL. With the rail down the wheel has no supply, so a low reading says
    // nothing about the VOL line itself; only call it open when VBAT is up.
    int raw = -1, pct = 0;
    app_sm_vol_wheel_read(&raw, &pct);
    out->sig[HARNESS_SIG_VOL].value = raw;
    if (raw < 0) {
        out->sig[HARNESS_SIG_VOL].state = HARNESS_UNKNOWN;
    } else if (raw >= VOL_OPEN_RAW) {
        out->sig[HARNESS_SIG_VOL].state = HARNESS_OK;
    } else {
        out->sig[HARNESS_SIG_VOL].state =
            out->installed ? HARNESS_OPEN : HARNESS_AMBIGUOUS;
    }

    // PAIR. A floating pad reads LOW exactly like a held button; only the dwell
    // separates them.
    int64_t low_ms = pair_low_ms();
    out->sig[HARNESS_SIG_PAIR].value = (gpio_get_level(PIN_CP_BUTTON) == 0) ? 0 : 1;
    if (!s_sampler_running) {
        out->sig[HARNESS_SIG_PAIR].state = HARNESS_UNKNOWN;
    } else if (low_ms < 0) {
        out->sig[HARNESS_SIG_PAIR].state = HARNESS_OK;   // idle HIGH, wire is on
    } else if (low_ms >= PAIR_STUCK_MS || !out->installed) {
        // Past the dwell it is stuck by duration. Short of that, an absent
        // battery rail settles it anyway: nobody is holding the shoulder button
        // of a Game Boy this board is not plugged into. Without the second test
        // the boot report, which runs seconds in, could never call PAIR at all.
        out->sig[HARNESS_SIG_PAIR].state = HARNESS_OPEN;
    } else {
        out->sig[HARNESS_SIG_PAIR].state = HARNESS_AMBIGUOUS;  // could still be a press
    }

    // HP detect. R10 holds this HIGH on the mod PCB, so HIGH means either
    // headphones or an open SW line. Bench power settles it.
    int hp_level = gpio_get_level(PIN_HP_DETECT);
    out->sig[HARNESS_SIG_HP].value = hp_level;
    if (hp_level == 0) {
        out->sig[HARNESS_SIG_HP].state = HARNESS_OK;   // GBA is grounding it
    } else {
        out->sig[HARNESS_SIG_HP].state =
            out->installed ? HARNESS_AMBIGUOUS : HARNESS_OPEN;
    }
}

const char *harness_state_name(harness_state_t st)
{
    switch (st) {
    case HARNESS_OK:        return "ok";
    case HARNESS_OPEN:      return "open";
    case HARNESS_AMBIGUOUS: return "unclear";
    default:                return "--";
    }
}

const char *harness_sig_name(harness_sig_t sig)
{
    switch (sig) {
    case HARNESS_SIG_VBAT: return "VBAT";
    case HARNESS_SIG_VOL:  return "VOL";
    case HARNESS_SIG_PAIR: return "PAIR";
    case HARNESS_SIG_HP:   return "HP";
    default:               return "?";
    }
}

const char *harness_sig_detail(const harness_report_t *rep, harness_sig_t sig)
{
    if (!rep || sig >= HARNESS_SIG_COUNT) return "";
    harness_state_t st = rep->sig[sig].state;

    switch (sig) {
    case HARNESS_SIG_VBAT:
        if (st == HARNESS_UNKNOWN) return "battery sense unavailable (ADC cali failed)";
        if (st == HARNESS_OPEN)    return "battery rail absent, running on external power (FPC1 pins 11,12)";
        return "battery rail present";

    case HARNESS_SIG_VOL:
        if (st == HARNESS_UNKNOWN)   return "VOL ADC unavailable";
        if (st == HARNESS_OPEN)      return "wheel line open, wheel ignored (check FPC1 pin 7)";
        if (st == HARNESS_AMBIGUOUS) return "reads low, but the wheel is fed from the absent VBAT rail";
        return "wheel reads in range";

    case HARNESS_SIG_PAIR:
        if (st == HARNESS_UNKNOWN)   return "not sampled yet";
        if (st == HARNESS_OPEN)      return "reads pressed with nothing to press it: R shoulder wire open (FPC1 pin 9 to flex J4)";
        if (st == HARNESS_AMBIGUOUS) return "reads pressed, could still be a real hold";
        return "idle, wire is connected";

    case HARNESS_SIG_HP:
        if (st == HARNESS_OPEN)      return "stuck plugged, HP sense line open (check FPC1 pin 8)";
        if (st == HARNESS_AMBIGUOUS) return "reads plugged: headphones in, or the sense line is open";
        return "headphone jack sensing correctly";

    default:
        return "";
    }
}

void harness_log_report(const harness_report_t *rep)
{
    if (!rep) return;

    if (rep->installed) {
        ESP_LOGI(TAG, "install check: connected to a Game Boy");
    } else {
        ESP_LOGW(TAG, "install check: NOT connected to a Game Boy "
                      "(no battery rail; bench or programming-header power)");
    }

    for (int i = 0; i < HARNESS_SIG_COUNT; i++) {
        harness_state_t st = rep->sig[i].state;
        int32_t v = rep->sig[i].value;
        // Units differ per signal, so each gets its own value rendering.
        char val[24];
        switch (i) {
        case HARNESS_SIG_VBAT:
            if (v < 0) snprintf(val, sizeof(val), "--");
            else       snprintf(val, sizeof(val), "%ld mV", (long)v);
            break;
        case HARNESS_SIG_VOL:
            if (v < 0) snprintf(val, sizeof(val), "--");
            else       snprintf(val, sizeof(val), "raw %ld", (long)v);
            break;
        default:
            snprintf(val, sizeof(val), "%s", v ? "high" : "low");
            break;
        }

        const char *detail = harness_sig_detail(rep, (harness_sig_t)i);
        if (st == HARNESS_OK) {
            ESP_LOGI(TAG, "  %-4s %-7s %-9s %s",
                     harness_sig_name((harness_sig_t)i),
                     harness_state_name(st), val, detail);
        } else {
            ESP_LOGW(TAG, "  %-4s %-7s %-9s %s",
                     harness_sig_name((harness_sig_t)i),
                     harness_state_name(st), val, detail);
        }
    }
}

size_t harness_pack(const harness_report_t *rep, uint8_t *buf, size_t buflen)
{
    if (!rep || !buf || buflen < HARNESS_PACKED_LEN) return 0;

    size_t n = 0;
    buf[n++] = HARNESS_WIRE_VER;
    buf[n++] = rep->installed ? 1 : 0;
    buf[n++] = (uint8_t)HARNESS_SIG_COUNT;
    for (int i = 0; i < HARNESS_SIG_COUNT; i++) {
        buf[n++] = (uint8_t)(rep->sig[i].state & 0x0F);
    }
    for (int i = 0; i < HARNESS_SIG_COUNT; i++) {
        int32_t v = rep->sig[i].value;
        buf[n++] = (uint8_t)(v         & 0xFF);
        buf[n++] = (uint8_t)((v >>  8) & 0xFF);
        buf[n++] = (uint8_t)((v >> 16) & 0xFF);
        buf[n++] = (uint8_t)((v >> 24) & 0xFF);
    }
    return n;
}
