/*
 * Scenario files: a timed script of synthetic device and staff actions.
 *
 *   # comment
 *   <t_ms> RAISE bed=12 device=pulse-ox-12 code=SPO2_LOW prio=HIGH value=86
 *   <t_ms> CLEAR bed=12 device=pulse-ox-12 code=SPO2_LOW
 *   <t_ms> ACK   alarm=1 who="R. Okafor (synthetic)" role=CHARGE_RN note="at bedside"
 *                [seen=stale] [request=42] [hash=0000000000000000]
 *   <t_ms> END
 *
 * An ACK reads the alarm's current version and hash first, the way a nurse
 * reads the console, unless seen=stale (uses the previous version, to show
 * the gate refusing) or hash= is given.
 */
#ifndef BG_SCENARIO_H
#define BG_SCENARIO_H

#include <stdint.h>
#include <stdio.h>

#include "bg/alarm.h"

#define BG_SCN_MAX_STEPS 512

typedef enum { BG_STEP_RAISE, BG_STEP_CLEAR, BG_STEP_ACK, BG_STEP_END } bg_step_kind;

typedef struct {
    uint64_t t_ms;
    bg_step_kind kind;
    char bed[BG_BED_LEN];
    char device[BG_DEV_LEN];
    char code[BG_CODE_LEN];
    bg_prio prio;
    int32_t value;
    uint32_t alarm_id;
    char who[BG_WHO_LEN];
    bg_role role;
    char note[BG_NOTE_LEN];
    int seen_stale;
    char hash16[BG_HASH16_LEN + 1];
    uint64_t request_id;
    int line;
} bg_step;

typedef struct {
    bg_step steps[BG_SCN_MAX_STEPS];
    int n;
    char error[160];
} bg_scenario;

int bg_scenario_parse(FILE *fp, bg_scenario *sc);
int bg_scenario_load(const char *path, bg_scenario *sc);

/* Run a scenario against an engine with simulated time; every log line
 * goes to the engine's log. Writes "t_ms TYPE payload" lines to out. */
int bg_scenario_run_offline(const bg_scenario *sc, FILE *out);

#endif
