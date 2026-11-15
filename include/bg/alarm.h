/*
 * Alarm escalation engine.
 *
 * Pure logic: no I/O except the event log, no threads, no malloc. Time is
 * passed in (milliseconds on a monotonic clock), so the same code runs in
 * the daemon, in the offline scenario runner and in unit tests.
 */
#ifndef BG_ALARM_H
#define BG_ALARM_H

#include <stdint.h>

#include "bg/eventlog.h"

#define BG_MAX_ALARMS 256
#define BG_BED_LEN 16
#define BG_DEV_LEN 24
#define BG_CODE_LEN 24
#define BG_WHO_LEN 48
#define BG_NOTE_LEN 160
#define BG_HASH16_LEN 16 /* characters of the version hash shown to people */

typedef enum { BG_PRIO_LOW = 1, BG_PRIO_MEDIUM = 2, BG_PRIO_HIGH = 3 } bg_prio;

typedef enum {
    BG_ST_FREE = 0,
    BG_ST_ACTIVE,    /* sounding, nobody has acknowledged it */
    BG_ST_ESCALATED, /* sounding and passed to the next tier */
    BG_ST_ACKED,     /* a named person has acknowledged it, condition may persist */
    BG_ST_RESOLVED   /* device reported the condition cleared */
} bg_state;

/* Tiers, in order. A role may acknowledge an alarm at its own tier or below. */
typedef enum {
    BG_ROLE_NONE = 0,
    BG_ROLE_BEDSIDE_RN = 1,
    BG_ROLE_CHARGE_RN = 2,
    BG_ROLE_RAPID_RESPONSE = 3
} bg_role;

typedef enum {
    BG_OK = 0,
    BG_OK_DUPLICATE = 1, /* same request seen before; nothing changed */
    BG_ERR_FULL = -1,
    BG_ERR_NOT_FOUND = -2,
    BG_ERR_STALE = -3,   /* alarm changed since the person looked at it */
    BG_ERR_ROLE = -4,    /* role is below the alarm's current tier */
    BG_ERR_NO_NAME = -5, /* acknowledgements must name a person */
    BG_ERR_STATE = -6,   /* e.g. acknowledging a resolved alarm */
    BG_ERR_INVALID = -7
} bg_status;

/* Escalation timing per priority. Demo values, not clinical guidance; a
 * real unit sets these from its own alarm policy. 0 = never escalate. */
typedef struct {
    uint32_t escalate_ms[3]; /* window at tier 1, 2, 3 before moving up */
    uint32_t rearm_ms;       /* acked but still present: sound again after */
} bg_policy;

typedef struct {
    uint32_t id;
    bg_state state;
    uint32_t version;
    char bed[BG_BED_LEN];
    char device[BG_DEV_LEN];
    char code[BG_CODE_LEN];
    bg_prio prio;
    int32_t value;
    uint32_t repeats;
    uint8_t tier; /* bg_role currently responsible */
    uint64_t raised_ms, last_seen_ms, deadline_ms, acked_ms, resolved_ms;
    char acked_by[BG_WHO_LEN];
    bg_role acked_role;
    char ack_note[BG_NOTE_LEN];
    uint64_t ack_request_id;
    uint8_t incident_filed;
    char vhash[BG_SHA256_HEX_LEN + 1]; /* hash of the alarm as displayed */
} bg_alarm;

typedef enum {
    BG_EV_RAISED,
    BG_EV_REPEAT,
    BG_EV_UPGRADED,
    BG_EV_ESCALATED,
    BG_EV_ACKED,
    BG_EV_REARMED,
    BG_EV_RESOLVED,
    BG_EV_INCIDENT
} bg_event_kind;

typedef void (*bg_notify_fn)(void *ctx, bg_event_kind kind, const bg_alarm *a, uint64_t now_ms);

typedef struct {
    bg_alarm alarms[BG_MAX_ALARMS];
    uint32_t next_id;
    bg_policy policy[4]; /* indexed by bg_prio */
    uint32_t window_scale_pct;
    bg_log *log;
    bg_notify_fn notify;
    void *notify_ctx;
    uint64_t incidents;
} bg_engine;

void bg_engine_init(bg_engine *e, bg_log *log);
/* Scale every escalation and re-arm window (100 = as configured). */
void bg_engine_set_scale(bg_engine *e, uint32_t pct);

/* A device reports a condition. Repeats of an open alarm for the same bed,
 * device and code fold into it. *out_id receives the alarm id. */
bg_status bg_raise(bg_engine *e, uint64_t now, const char *bed, const char *device,
                   const char *code, bg_prio prio, int32_t value, uint32_t *out_id);
/* The device reports the condition has cleared. */
bg_status bg_clear(bg_engine *e, uint64_t now, const char *bed, const char *device,
                   const char *code, uint32_t *out_id);
/* A person acknowledges what they saw: alarm id, version and the first
 * BG_HASH16_LEN hex characters of the version hash. */
bg_status bg_ack(bg_engine *e, uint64_t now, uint32_t id, uint32_t version, const char *hash16,
                 const char *who, bg_role role, const char *note, uint64_t request_id);
/* Advance time: escalations and re-arms. Returns how many alarms changed. */
int bg_tick(bg_engine *e, uint64_t now);
/* Earliest pending deadline, or UINT64_MAX. */
uint64_t bg_next_deadline(const bg_engine *e);

const bg_alarm *bg_find(const bg_engine *e, uint32_t id);
int bg_open_count(const bg_engine *e);

const char *bg_prio_name(bg_prio p);
const char *bg_state_name(bg_state s);
const char *bg_role_name(bg_role r);
const char *bg_status_name(bg_status s);
int bg_prio_parse(const char *s, bg_prio *out);
int bg_role_parse(const char *s, bg_role *out);

#endif
