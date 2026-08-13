#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Install diagnostics for the four signals that cross from the mod PCB to the
// Game Boy through FPC1 and the flex. Each is classified independently so a
// partial misconnection (one flex pin not seated, one wire unsoldered) reads
// differently from a board that is simply on the bench.
//
// The signatures below are measured, not assumed. A bare board on the
// Tag-Connect header reads VBAT 284 mV and VOL raw 16; an installed board reads
// VBAT 2000..3200 mV and VOL raw 330..3600. The thresholds sit in the gap.
//
// Signal-by-signal, and why each one looks the way it does:
//
//   VBAT (FPC1 pins 11, 12) -- the R20/R21 divider is local to the mod PCB but
//     the rail it measures arrives only through the flex. The CPU running while
//     VBAT reads near zero is a contradiction with one explanation: the board is
//     powered from the programming header. This is the primary verdict signal.
//
//   VOL (FPC1 pin 7) -- the wheel is a VBAT-referenced divider, so its wiper
//     cannot reach ground while connected. A reading near zero means the line is
//     open (or VBAT is, which the report says instead when that is the case).
//
//   PAIR (FPC1 pin 9, then a hand-soldered wire to J4 and the GBA R shoulder) --
//     the only one of the four with no pull-up anywhere on either board and no
//     internal pull on GPIO34. It floats when the wire is off, and a floating
//     pad reads LOW, i.e. indistinguishable from a held button except by how
//     long it stays that way. Nobody holds R for 20 s.
//
//   HP detect (FPC1 pin 8) -- R10 10k to +3V3 sits on the mod PCB, upstream of
//     the connector, so it reads HIGH with the flex removed. That is the same
//     level as headphones-plugged, so this signal cannot be judged alone. It is
//     reported AMBIGUOUS unless VBAT says we are on bench power, where
//     headphones-plugged is implausible and it resolves to OPEN.

typedef enum {
    HARNESS_OK = 0,      // plausible installed reading
    HARNESS_OPEN,        // matches the open-circuit signature
    HARNESS_AMBIGUOUS,   // reading cannot separate installed from open on its own
    HARNESS_UNKNOWN,     // sense unavailable (ADC down, or not sampled yet)
} harness_state_t;

typedef enum {
    HARNESS_SIG_VBAT = 0,
    HARNESS_SIG_VOL,
    HARNESS_SIG_PAIR,
    HARNESS_SIG_HP,
    HARNESS_SIG_COUNT,
} harness_sig_t;

typedef struct {
    harness_state_t state;
    int32_t         value;   // VBAT millivolts / VOL raw counts / pin level 0-1
} harness_sig_status_t;

typedef struct {
    bool                 installed;   // false when the battery rail is absent
    harness_sig_status_t sig[HARNESS_SIG_COUNT];
} harness_report_t;

// Subscribe to report changes. Fires on the sampler task whenever any signal's
// state flips, so a subscriber can push the new report without polling. One
// subscriber; a second call replaces the first. Safe to call before
// harness_init(). The callback must not block for long.
typedef void (*harness_change_cb_t)(const harness_report_t *rep);
void harness_set_change_cb(harness_change_cb_t cb);

// Start the 1 Hz sampler that tracks how long PAIR has been continuously LOW.
// Call after buttons_init(); the ADC-backed signals need app_sm_prime_volume()
// to have run, which main.c does far earlier.
esp_err_t harness_init(void);

// Evaluate all four signals now. Cheap: one VBAT read, one VOL read, two pin
// reads. Safe before harness_init(), where PAIR reports UNKNOWN (no dwell yet).
void harness_eval(harness_report_t *out);

// True when the battery rail reads present. The factory-reset gate gets its
// answer from here: with no rail the board cannot be installed in a Game Boy,
// so a Connect/Pair hold at boot is a floating pin, not a user request.
bool harness_rail_present(void);

// True when the VOL line reads open, so the wheel poll must not drive volume.
bool harness_vol_open(void);

// Short name for a state ("ok" / "open" / "unclear" / "--").
const char *harness_state_name(harness_state_t st);

// Short name for a signal ("VBAT" / "VOL" / "PAIR" / "HP").
const char *harness_sig_name(harness_sig_t sig);

// One line of operator-facing detail for a signal: what was measured and, when
// something is wrong, which connector pin to check. Never NULL.
const char *harness_sig_detail(const harness_report_t *rep, harness_sig_t sig);

// Log the full report at INFO, one line per signal. Used for the boot-time QA
// block and the `diag` console command.
void harness_log_report(const harness_report_t *rep);

// Pack the report for the BLE diagnostics characteristic. Returns bytes written;
// buffer must be at least HARNESS_PACKED_LEN.
//
// Wire format, little-endian:
//   [0]        format version (HARNESS_WIRE_VER)
//   [1]        installed flag, 0 or 1
//   [2]        signal count N
//   [3 .. 3+N) one state byte per signal, in harness_sig_t order
//   [3+N ..)   N int32 values, same order
//
// The explicit count is what makes adding a signal a non-breaking change: a
// reader computes the value offset as 3+N rather than assuming its own count,
// so an older page parses the signals it knows and ignores the rest. Same
// contract the settings characteristic keeps. Append new signals at the end of
// harness_sig_t only; reordering breaks every existing reader.
#define HARNESS_WIRE_VER   1
#define HARNESS_PACKED_LEN (3 + HARNESS_SIG_COUNT + HARNESS_SIG_COUNT * 4)
size_t harness_pack(const harness_report_t *rep, uint8_t *buf, size_t buflen);
