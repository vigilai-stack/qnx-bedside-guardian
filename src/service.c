#include "bg/service.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void bg_json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return;
    for (; in && *in && o + 7 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (ch < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

static void console_line(bg_service *s, uint64_t now, const char *what, const bg_alarm *a) {
    if (s->console == NULL) return;
    fprintf(s->console, "[%7.1fs] %-9s #%-3" PRIu32 " bed %-4s %-16s %-6s %-10s tier=%-15s v%" PRIu32
            " %.16s",
            (double)now / 1000.0, what, a->id, a->bed, a->code, bg_prio_name(a->prio),
            bg_state_name(a->state), bg_role_name((bg_role)a->tier), a->version, a->vhash);
    if (a->repeats) fprintf(s->console, "  (x%" PRIu32 ")", a->repeats + 1);
    if (a->state == BG_ST_ACKED && a->acked_by[0])
        fprintf(s->console, "  by %s: \"%s\"", a->acked_by, a->ack_note);
    fputc('\n', s->console);
    fflush(s->console);
}

static void write_incident(bg_service *s, const bg_alarm *a, uint64_t now, const char *event) {
    char bed[64], dev[64], code[64], who[2 * BG_WHO_LEN], note[2 * BG_NOTE_LEN];
    if (s->incidents == NULL) return;
    bg_json_escape(a->bed, bed, sizeof bed);
    bg_json_escape(a->device, dev, sizeof dev);
    bg_json_escape(a->code, code, sizeof code);
    bg_json_escape(a->acked_by, who, sizeof who);
    bg_json_escape(a->ack_note, note, sizeof note);
    if (strcmp(event, "opened") == 0) {
        fprintf(s->incidents,
                "{\"schema\":\"bedside-guardian.incident.v1\",\"event\":\"opened\","
                "\"synthetic\":true,\"category\":\"patient_safety\","
                "\"incident_type\":\"unacknowledged_alarm\",\"alarm_id\":%" PRIu32
                ",\"bed\":\"%s\",\"device\":\"%s\",\"code\":\"%s\",\"priority\":\"%s\","
                "\"raised_t_ms\":%" PRIu64 ",\"opened_t_ms\":%" PRIu64
                ",\"repeats\":%" PRIu32 ",\"log_head\":\"%s\","
                "\"summary\":\"%s %s alarm at bed %s was not acknowledged for %.1f s and "
                "reached the rapid response tier.\"}\n",
                a->id, bed, dev, code, bg_prio_name(a->prio), a->raised_ms, now, a->repeats,
                s->log.head, bg_prio_name(a->prio), code, bed,
                (double)(now - a->raised_ms) / 1000.0);
    } else {
        fprintf(s->incidents,
                "{\"schema\":\"bedside-guardian.incident.v1\",\"event\":\"%s\",\"alarm_id\":%" PRIu32
                ",\"t_ms\":%" PRIu64 ",\"by\":\"%s\",\"role\":\"%s\",\"note_verbatim\":\"%s\","
                "\"log_head\":\"%s\"}\n",
                event, a->id, now, who, a->acked_by[0] ? bg_role_name(a->acked_role) : "",
                note, s->log.head);
    }
    fflush(s->incidents);
}

static void on_event(void *ctx, bg_event_kind k, const bg_alarm *a, uint64_t now) {
    bg_service *s = ctx;
    static const char *names[] = {"RAISE",  "REPEAT",  "UPGRADE", "ESCALATE",
                                  "ACK",    "REARM",   "RESOLVE", "INCIDENT"};
    console_line(s, now, names[k], a);
    if (k == BG_EV_INCIDENT) write_incident(s, a, now, "opened");
    else if (a->incident_filed && k == BG_EV_ACKED) write_incident(s, a, now, "acknowledged");
    else if (a->incident_filed && k == BG_EV_RESOLVED) write_incident(s, a, now, "resolved");
}

int bg_service_init(bg_service *s, const char *log_path, const char *incidents_path,
                    FILE *console, int sync) {
    char when[64];
    time_t t = time(NULL);
    struct tm tmv;
    memset(s, 0, sizeof *s);
    if (bg_log_open(&s->log, log_path, sync) != 0) return -1;
    if (incidents_path) {
        s->incidents = fopen(incidents_path, "a");
        if (s->incidents == NULL) {
            bg_log_close(&s->log);
            return -1;
        }
    }
    s->console = console;
    bg_engine_init(&s->eng, &s->log);
    s->eng.notify = on_event;
    s->eng.notify_ctx = s;
    gmtime_r(&t, &tmv);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    bg_log_append(&s->log, 0, "START", "wall=%s data=synthetic", when);
    return 0;
}

static void fill_alarm_reply(const bg_alarm *a, bg_reply *r) {
    r->alarm_id = a->id;
    r->version = a->version;
    r->state = (uint16_t)a->state;
    r->tier = a->tier;
    memcpy(r->hash16, a->vhash, BG_HASH16_LEN);
    r->hash16[BG_HASH16_LEN] = '\0';
}

static void describe(const bg_alarm *a, char *out, size_t cap) {
    snprintf(out, cap,
             "#%" PRIu32 " bed %s %s %s %s value=%" PRId32 " %s tier=%s repeats=%" PRIu32
             " v%" PRIu32 " %.16s%s%s%s",
             a->id, a->bed, a->device, a->code, bg_prio_name(a->prio), a->value,
             bg_state_name(a->state), bg_role_name((bg_role)a->tier), a->repeats, a->version,
             a->vhash, a->acked_by[0] ? " acked_by=\"" : "", a->acked_by,
             a->acked_by[0] ? "\"" : "");
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

void bg_service_latency(const bg_service *s, bg_latency *out) {
    static uint64_t tmp[BG_LAT_SAMPLES];
    size_t n = s->lat_count < BG_LAT_SAMPLES ? (size_t)s->lat_count : BG_LAT_SAMPLES;
    memset(out, 0, sizeof *out);
    out->count = s->lat_count;
    out->max_ns = s->lat_max_ns;
    if (n == 0) return;
    memcpy(tmp, s->lat_ns, n * sizeof tmp[0]);
    qsort(tmp, n, sizeof tmp[0], cmp_u64);
    out->p50_ns = tmp[(n - 1) / 2];
    out->p99_ns = tmp[(n * 99 + 99) / 100 - 1];
}

void bg_service_handle(bg_service *s, uint64_t now_ms, uint64_t latency_ns, const bg_msg *m,
                       bg_reply *r) {
    char bed[BG_BED_LEN], dev[BG_DEV_LEN], code[BG_CODE_LEN], who[BG_WHO_LEN],
        hash[BG_HASH16_LEN + 1], note[BG_NOTE_LEN];
    uint32_t id = 0;
    const bg_alarm *a;
    memset(r, 0, sizeof *r);
    if (m->magic != BG_MAGIC) {
        r->status = BG_ERR_INVALID;
        snprintf(r->text, sizeof r->text, "bad magic");
        return;
    }
    /* Never trust NUL termination from the wire. */
    snprintf(bed, sizeof bed, "%.*s", (int)sizeof m->bed - 1, m->bed);
    snprintf(dev, sizeof dev, "%.*s", (int)sizeof m->device - 1, m->device);
    snprintf(code, sizeof code, "%.*s", (int)sizeof m->code - 1, m->code);
    snprintf(who, sizeof who, "%.*s", (int)sizeof m->who - 1, m->who);
    snprintf(hash, sizeof hash, "%.*s", (int)sizeof m->hash16 - 1, m->hash16);
    snprintf(note, sizeof note, "%.*s", (int)sizeof m->note - 1, m->note);

    switch (m->type) {
    case BG_MSG_RAISE:
        if (latency_ns) {
            s->lat_ns[s->lat_count % BG_LAT_SAMPLES] = latency_ns;
            s->lat_count++;
            if (latency_ns > s->lat_max_ns) s->lat_max_ns = latency_ns;
        }
        r->status = bg_raise(&s->eng, now_ms, bed, dev, code, (bg_prio)m->prio, m->value, &id);
        break;
    case BG_MSG_CLEAR:
        r->status = bg_clear(&s->eng, now_ms, bed, dev, code, &id);
        break;
    case BG_MSG_ACK:
        id = m->alarm_id;
        r->status = bg_ack(&s->eng, now_ms, id, m->version, hash, who, (bg_role)m->role, note,
                           m->request_id);
        break;
    case BG_MSG_GET:
        id = m->alarm_id;
        r->status = bg_find(&s->eng, id) ? BG_OK : BG_ERR_NOT_FOUND;
        break;
    case BG_MSG_STATUS: {
        bg_latency lat;
        size_t used = 0;
        bg_service_latency(s, &lat);
        used += (size_t)snprintf(r->text, sizeof r->text,
                                 "open=%d incidents=%" PRIu64 " events=%" PRIu64
                                 " raise_latency_us p50=%.1f p99=%.1f max=%.1f n=%" PRIu64 "\n",
                                 bg_open_count(&s->eng), s->eng.incidents, s->log.seq,
                                 (double)lat.p50_ns / 1e3, (double)lat.p99_ns / 1e3,
                                 (double)lat.max_ns / 1e3, lat.count);
        for (int i = 0; i < BG_MAX_ALARMS && used + 200 < sizeof r->text; i++) {
            const bg_alarm *x = &s->eng.alarms[i];
            if (x->state == BG_ST_ACTIVE || x->state == BG_ST_ESCALATED ||
                x->state == BG_ST_ACKED) {
                describe(x, r->text + used, sizeof r->text - used - 1);
                used += strlen(r->text + used);
                r->text[used++] = '\n';
                r->text[used] = '\0';
            }
        }
        r->status = BG_OK;
        return;
    }
    case BG_MSG_SHUTDOWN:
        s->shutdown_requested = 1;
        r->status = BG_OK;
        snprintf(r->text, sizeof r->text, "shutting down");
        return;
    default:
        r->status = BG_ERR_INVALID;
        snprintf(r->text, sizeof r->text, "unknown message type %u", m->type);
        return;
    }
    a = bg_find(&s->eng, id);
    if (a) {
        fill_alarm_reply(a, r);
        describe(a, r->text, sizeof r->text);
    } else if (r->status < 0) {
        snprintf(r->text, sizeof r->text, "%s", bg_status_name((bg_status)r->status));
    }
}

void bg_service_close(bg_service *s, uint64_t now_ms) {
    bg_latency lat;
    bg_service_latency(s, &lat);
    bg_log_append(&s->log, now_ms, "STOP",
                  "open=%d incidents=%" PRIu64 " raise_latency_ns p50=%" PRIu64 " p99=%" PRIu64
                  " max=%" PRIu64 " n=%" PRIu64,
                  bg_open_count(&s->eng), s->eng.incidents, lat.p50_ns, lat.p99_ns, lat.max_ns,
                  lat.count);
    bg_log_close(&s->log);
    if (s->incidents) fclose(s->incidents);
    s->incidents = NULL;
}
