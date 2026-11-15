#include "bg/alarm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NEVER UINT64_MAX

static void copy_str(char *dst, size_t cap, const char *src) {
    if (src == NULL) src = "";
    snprintf(dst, cap, "%s", src);
}

const char *bg_prio_name(bg_prio p) {
    switch (p) {
    case BG_PRIO_LOW: return "LOW";
    case BG_PRIO_MEDIUM: return "MEDIUM";
    case BG_PRIO_HIGH: return "HIGH";
    }
    return "?";
}

const char *bg_state_name(bg_state s) {
    switch (s) {
    case BG_ST_FREE: return "FREE";
    case BG_ST_ACTIVE: return "ACTIVE";
    case BG_ST_ESCALATED: return "ESCALATED";
    case BG_ST_ACKED: return "ACKED";
    case BG_ST_RESOLVED: return "RESOLVED";
    }
    return "?";
}

const char *bg_role_name(bg_role r) {
    switch (r) {
    case BG_ROLE_NONE: return "NONE";
    case BG_ROLE_BEDSIDE_RN: return "BEDSIDE_RN";
    case BG_ROLE_CHARGE_RN: return "CHARGE_RN";
    case BG_ROLE_RAPID_RESPONSE: return "RAPID_RESPONSE";
    }
    return "?";
}

const char *bg_status_name(bg_status s) {
    switch (s) {
    case BG_OK: return "OK";
    case BG_OK_DUPLICATE: return "OK_DUPLICATE";
    case BG_ERR_FULL: return "ERR_FULL";
    case BG_ERR_NOT_FOUND: return "ERR_NOT_FOUND";
    case BG_ERR_STALE: return "ERR_STALE";
    case BG_ERR_ROLE: return "ERR_ROLE";
    case BG_ERR_NO_NAME: return "ERR_NO_NAME";
    case BG_ERR_STATE: return "ERR_STATE";
    case BG_ERR_INVALID: return "ERR_INVALID";
    }
    return "?";
}

int bg_prio_parse(const char *s, bg_prio *out) {
    if (strcasecmp(s, "HIGH") == 0) *out = BG_PRIO_HIGH;
    else if (strcasecmp(s, "MEDIUM") == 0) *out = BG_PRIO_MEDIUM;
    else if (strcasecmp(s, "LOW") == 0) *out = BG_PRIO_LOW;
    else return -1;
    return 0;
}

int bg_role_parse(const char *s, bg_role *out) {
    if (strcasecmp(s, "BEDSIDE_RN") == 0) *out = BG_ROLE_BEDSIDE_RN;
    else if (strcasecmp(s, "CHARGE_RN") == 0) *out = BG_ROLE_CHARGE_RN;
    else if (strcasecmp(s, "RAPID_RESPONSE") == 0) *out = BG_ROLE_RAPID_RESPONSE;
    else return -1;
    return 0;
}

void bg_engine_init(bg_engine *e, bg_log *log) {
    memset(e, 0, sizeof *e);
    e->next_id = 1;
    e->log = log;
    e->window_scale_pct = 100;
    /* Demo policy. HIGH: bedside 15 s, charge 15 s, then rapid response is
     * re-paged every 30 s. MEDIUM: bedside 60 s, then stays with the charge nurse.
     * LOW: informational, never escalates. */
    e->policy[BG_PRIO_HIGH] = (bg_policy){{15000, 15000, 30000}, 60000};
    e->policy[BG_PRIO_MEDIUM] = (bg_policy){{60000, 0, 0}, 180000};
    e->policy[BG_PRIO_LOW] = (bg_policy){{0, 0, 0}, 0};
}

void bg_engine_set_scale(bg_engine *e, uint32_t pct) {
    e->window_scale_pct = pct ? pct : 1;
}

static uint64_t scaled(const bg_engine *e, uint32_t ms) {
    uint64_t v = (uint64_t)ms * e->window_scale_pct / 100u;
    return v ? v : 1;
}

/* Deadline for the window at the alarm's current tier, or NEVER. */
static uint64_t tier_deadline(const bg_engine *e, const bg_alarm *a, uint64_t now) {
    uint32_t w = e->policy[a->prio].escalate_ms[a->tier - 1];
    return w ? now + scaled(e, w) : NEVER;
}

static void rehash(bg_alarm *a) {
    char buf[256];
    uint8_t d[BG_SHA256_LEN];
    int n = snprintf(buf, sizeof buf, "%" PRIu32 "|%" PRIu32 "|%s|%s|%s|%s|%s|%s", a->id,
                     a->version, a->bed, a->device, a->code, bg_prio_name(a->prio),
                     bg_state_name(a->state), bg_role_name((bg_role)a->tier));
    bg_sha256(buf, (size_t)n, d);
    bg_hex(d, sizeof d, a->vhash);
}

static void bump(bg_alarm *a) {
    a->version++;
    rehash(a);
}

static void emit(bg_engine *e, bg_event_kind k, const bg_alarm *a, uint64_t now) {
    if (e->notify) e->notify(e->notify_ctx, k, a, now);
}

static void logf_alarm(bg_engine *e, uint64_t now, const char *type, const bg_alarm *a,
                       const char *extra) {
    if (e->log == NULL) return;
    bg_log_append(e->log, now, type,
                  "alarm=%" PRIu32 " v=%" PRIu32 " bed=%s device=%s code=%s prio=%s value=%" PRId32
                  " state=%s tier=%s repeats=%" PRIu32 " vhash=%.16s%s%s",
                  a->id, a->version, a->bed, a->device, a->code, bg_prio_name(a->prio), a->value,
                  bg_state_name(a->state), bg_role_name((bg_role)a->tier), a->repeats, a->vhash,
                  extra && *extra ? " " : "", extra ? extra : "");
}

static int is_open(const bg_alarm *a) {
    return a->state == BG_ST_ACTIVE || a->state == BG_ST_ESCALATED || a->state == BG_ST_ACKED;
}

static bg_alarm *find_open(bg_engine *e, const char *bed, const char *device, const char *code) {
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        bg_alarm *a = &e->alarms[i];
        if (is_open(a) && strcmp(a->bed, bed) == 0 && strcmp(a->device, device) == 0 &&
            strcmp(a->code, code) == 0)
            return a;
    }
    return NULL;
}

static bg_alarm *free_slot(bg_engine *e) {
    bg_alarm *oldest = NULL;
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        bg_alarm *a = &e->alarms[i];
        if (a->state == BG_ST_FREE) return a;
        if (a->state == BG_ST_RESOLVED && (oldest == NULL || a->resolved_ms < oldest->resolved_ms))
            oldest = a;
    }
    return oldest;
}

bg_status bg_raise(bg_engine *e, uint64_t now, const char *bed, const char *device,
                   const char *code, bg_prio prio, int32_t value, uint32_t *out_id) {
    bg_alarm *a;
    if (!bed || !*bed || !device || !*device || !code || !*code) return BG_ERR_INVALID;
    if (prio < BG_PRIO_LOW || prio > BG_PRIO_HIGH) return BG_ERR_INVALID;

    a = find_open(e, bed, device, code);
    if (a) {
        a->repeats++;
        a->last_seen_ms = now;
        a->value = value;
        if (prio > a->prio) {
            /* Worse than before: treat as a new alarm cycle at the new priority. */
            a->prio = prio;
            a->state = BG_ST_ACTIVE;
            a->deadline_ms = tier_deadline(e, a, now);
            bump(a);
            logf_alarm(e, now, "UPGRADE", a, NULL);
            emit(e, BG_EV_UPGRADED, a, now);
        } else {
            /* Same condition again: count it, don't make new noise. */
            logf_alarm(e, now, "REPEAT", a, NULL);
            emit(e, BG_EV_REPEAT, a, now);
        }
        if (out_id) *out_id = a->id;
        return BG_OK;
    }

    a = free_slot(e);
    if (a == NULL) {
        if (e->log)
            bg_log_append(e->log, now, "CAPACITY",
                          "dropped bed=%s device=%s code=%s prio=%s: alarm table full", bed,
                          device, code, bg_prio_name(prio));
        return BG_ERR_FULL;
    }
    memset(a, 0, sizeof *a);
    a->id = e->next_id++;
    a->state = BG_ST_ACTIVE;
    copy_str(a->bed, sizeof a->bed, bed);
    copy_str(a->device, sizeof a->device, device);
    copy_str(a->code, sizeof a->code, code);
    bg_log_sanitize(a->bed);
    bg_log_sanitize(a->device);
    bg_log_sanitize(a->code);
    a->prio = prio;
    a->value = value;
    a->tier = BG_ROLE_BEDSIDE_RN;
    a->raised_ms = a->last_seen_ms = now;
    a->deadline_ms = tier_deadline(e, a, now);
    a->acked_ms = a->resolved_ms = 0;
    a->version = 0;
    bump(a);
    logf_alarm(e, now, "RAISE", a, NULL);
    emit(e, BG_EV_RAISED, a, now);
    if (out_id) *out_id = a->id;
    return BG_OK;
}

bg_status bg_clear(bg_engine *e, uint64_t now, const char *bed, const char *device,
                   const char *code, uint32_t *out_id) {
    bg_alarm *a = find_open(e, bed ? bed : "", device ? device : "", code ? code : "");
    if (a == NULL) return BG_ERR_NOT_FOUND;
    a->state = BG_ST_RESOLVED;
    a->resolved_ms = now;
    a->deadline_ms = NEVER;
    bump(a);
    logf_alarm(e, now, "RESOLVE", a, NULL);
    emit(e, BG_EV_RESOLVED, a, now);
    if (out_id) *out_id = a->id;
    return BG_OK;
}

bg_status bg_ack(bg_engine *e, uint64_t now, uint32_t id, uint32_t version, const char *hash16,
                 const char *who, bg_role role, const char *note, uint64_t request_id) {
    bg_alarm *a = (bg_alarm *)bg_find(e, id);
    char extra[BG_WHO_LEN + BG_NOTE_LEN + 96];
    bg_status refuse = BG_OK;

    if (a == NULL) return BG_ERR_NOT_FOUND;
    if (request_id != 0 && a->ack_request_id == request_id) {
        /* Same press delivered twice (retry, double tap): counted once. */
        if (e->log)
            bg_log_append(e->log, now, "ACK_DUPLICATE",
                          "alarm=%" PRIu32 " request=%" PRIu64 " already_recorded_at_v=%" PRIu32,
                          id, request_id, a->version);
        return BG_OK_DUPLICATE;
    }

    if (who == NULL || *who == '\0') refuse = BG_ERR_NO_NAME;
    else if (a->state == BG_ST_RESOLVED || a->state == BG_ST_ACKED) refuse = BG_ERR_STATE;
    else if (role < BG_ROLE_BEDSIDE_RN || role > BG_ROLE_RAPID_RESPONSE) refuse = BG_ERR_INVALID;
    else if ((int)role < (int)a->tier) refuse = BG_ERR_ROLE;
    else if (version != a->version || hash16 == NULL || strlen(hash16) < BG_HASH16_LEN ||
             strncmp(hash16, a->vhash, BG_HASH16_LEN) != 0)
        refuse = BG_ERR_STALE;

    if (refuse != BG_OK) {
        /* Refusals are part of the record too. */
        if (e->log) {
            char w[BG_WHO_LEN];
            copy_str(w, sizeof w, who);
            bg_log_append(e->log, now, "ACK_REFUSED",
                          "alarm=%" PRIu32 " asked_v=%" PRIu32 " current_v=%" PRIu32
                          " who=\"%s\" role=%s reason=%s",
                          id, version, a->version, w, bg_role_name(role), bg_status_name(refuse));
        }
        return refuse;
    }

    a->state = BG_ST_ACKED;
    a->acked_ms = now;
    a->acked_role = role;
    a->ack_request_id = request_id;
    copy_str(a->acked_by, sizeof a->acked_by, who);
    copy_str(a->ack_note, sizeof a->ack_note, note);
    {
        uint32_t r = e->policy[a->prio].rearm_ms;
        a->deadline_ms = r ? now + scaled(e, r) : NEVER;
    }
    bump(a);
    snprintf(extra, sizeof extra, "by=\"%s\" role=%s seen_v=%" PRIu32 " request=%" PRIu64
             " note=\"%s\"", a->acked_by, bg_role_name(role), version, request_id, a->ack_note);
    logf_alarm(e, now, "ACK", a, extra);
    emit(e, BG_EV_ACKED, a, now);
    return BG_OK;
}

int bg_tick(bg_engine *e, uint64_t now) {
    int changed = 0;
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        bg_alarm *a = &e->alarms[i];
        if (!is_open(a) || a->deadline_ms == NEVER || now < a->deadline_ms) continue;
        changed++;
        if (a->state == BG_ST_ACKED) {
            /* Acknowledged, but the device never said it cleared. */
            a->state = BG_ST_ACTIVE;
            a->deadline_ms = tier_deadline(e, a, now);
            bump(a);
            logf_alarm(e, now, "REARM", a, NULL);
            emit(e, BG_EV_REARMED, a, now);
            continue;
        }
        if (a->tier < BG_ROLE_RAPID_RESPONSE) {
            a->tier++;
            a->state = BG_ST_ESCALATED;
            a->deadline_ms = tier_deadline(e, a, now);
            bump(a);
            logf_alarm(e, now, "ESCALATE", a, NULL);
            emit(e, BG_EV_ESCALATED, a, now);
        } else {
            /* Top tier window passed with no acknowledgement: page again. */
            a->deadline_ms = tier_deadline(e, a, now);
            logf_alarm(e, now, "REPAGE", a, NULL);
            emit(e, BG_EV_ESCALATED, a, now);
        }
        if (a->tier == BG_ROLE_RAPID_RESPONSE && !a->incident_filed) {
            a->incident_filed = 1;
            e->incidents++;
            logf_alarm(e, now, "INCIDENT", a, "reason=reached_rapid_response_unacknowledged");
            emit(e, BG_EV_INCIDENT, a, now);
        }
    }
    return changed;
}

uint64_t bg_next_deadline(const bg_engine *e) {
    uint64_t best = NEVER;
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        const bg_alarm *a = &e->alarms[i];
        if (is_open(a) && a->deadline_ms < best) best = a->deadline_ms;
    }
    return best;
}

const bg_alarm *bg_find(const bg_engine *e, uint32_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        if (e->alarms[i].state != BG_ST_FREE && e->alarms[i].id == id) return &e->alarms[i];
    }
    return NULL;
}

int bg_open_count(const bg_engine *e) {
    int n = 0;
    for (int i = 0; i < BG_MAX_ALARMS; i++) n += is_open(&e->alarms[i]);
    return n;
}
