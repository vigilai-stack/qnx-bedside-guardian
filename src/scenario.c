#include "bg/scenario.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *next_token(const char *p, char *key, size_t kcap, char *val, size_t vcap) {
    size_t k = 0, v = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0' || *p == '#') return NULL;
    while (*p && *p != '=' && !isspace((unsigned char)*p)) {
        if (k + 1 < kcap) key[k++] = *p;
        p++;
    }
    key[k] = '\0';
    val[0] = '\0';
    if (*p != '=') return p;
    p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (v + 1 < vcap) val[v++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p)) {
            if (v + 1 < vcap) val[v++] = *p;
            p++;
        }
    }
    val[v] = '\0';
    return p;
}

/* Copy a value, refusing (not truncating) one that does not fit: a cut-off
 * bed or device name could silently match a different alarm. */
static int set_field(char *dst, size_t cap, const char *val) {
    size_t n = strlen(val);
    if (n >= cap) return -1;
    memcpy(dst, val, n + 1);
    return 0;
}

static int fail(bg_scenario *sc, int line, const char *msg) {
    snprintf(sc->error, sizeof sc->error, "line %d: %s", line, msg);
    return -1;
}

int bg_scenario_parse(FILE *fp, bg_scenario *sc) {
    char line[512];
    int lineno = 0;
    uint64_t last_t = 0;
    memset(sc, 0, sizeof *sc);
    while (fgets(line, sizeof line, fp)) {
        char key[32], val[BG_NOTE_LEN];
        const char *p = line;
        char *end;
        bg_step *st;
        lineno++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        if (sc->n >= BG_SCN_MAX_STEPS) return fail(sc, lineno, "too many steps");
        st = &sc->steps[sc->n];
        memset(st, 0, sizeof *st);
        st->line = lineno;
        st->t_ms = strtoull(p, &end, 10);
        if (end == p) return fail(sc, lineno, "expected a time in ms");
        if (st->t_ms < last_t) return fail(sc, lineno, "times must not go backwards");
        last_t = st->t_ms;
        p = next_token(end, key, sizeof key, val, sizeof val);
        if (p == NULL) return fail(sc, lineno, "missing action");
        if (strcmp(key, "RAISE") == 0) st->kind = BG_STEP_RAISE;
        else if (strcmp(key, "CLEAR") == 0) st->kind = BG_STEP_CLEAR;
        else if (strcmp(key, "ACK") == 0) st->kind = BG_STEP_ACK;
        else if (strcmp(key, "END") == 0) st->kind = BG_STEP_END;
        else return fail(sc, lineno, "unknown action");
        st->prio = BG_PRIO_HIGH;
        st->request_id = (uint64_t)lineno;
        while ((p = next_token(p, key, sizeof key, val, sizeof val)) != NULL) {
            if (strcmp(key, "bed") == 0) {
                if (set_field(st->bed, sizeof st->bed, val) != 0) return fail(sc, lineno, "bed too long");
            }
            else if (strcmp(key, "device") == 0) {
                if (set_field(st->device, sizeof st->device, val) != 0) return fail(sc, lineno, "device too long");
            }
            else if (strcmp(key, "code") == 0) {
                if (set_field(st->code, sizeof st->code, val) != 0) return fail(sc, lineno, "code too long");
            }
            else if (strcmp(key, "prio") == 0) {
                if (bg_prio_parse(val, &st->prio) != 0) return fail(sc, lineno, "bad prio");
            } else if (strcmp(key, "value") == 0) st->value = (int32_t)strtol(val, NULL, 10);
            else if (strcmp(key, "alarm") == 0) st->alarm_id = (uint32_t)strtoul(val, NULL, 10);
            else if (strcmp(key, "who") == 0) {
                if (set_field(st->who, sizeof st->who, val) != 0) return fail(sc, lineno, "who too long");
            }
            else if (strcmp(key, "role") == 0) {
                if (bg_role_parse(val, &st->role) != 0) return fail(sc, lineno, "bad role");
            } else if (strcmp(key, "note") == 0) {
                if (set_field(st->note, sizeof st->note, val) != 0) return fail(sc, lineno, "note too long");
            }
            else if (strcmp(key, "seen") == 0) st->seen_stale = strcmp(val, "stale") == 0;
            else if (strcmp(key, "hash") == 0) {
                if (set_field(st->hash16, sizeof st->hash16, val) != 0) return fail(sc, lineno, "hash too long");
            }
            else if (strcmp(key, "request") == 0) st->request_id = strtoull(val, NULL, 10);
            else return fail(sc, lineno, "unknown key");
        }
        if ((st->kind == BG_STEP_RAISE || st->kind == BG_STEP_CLEAR) &&
            (!st->bed[0] || !st->device[0] || !st->code[0]))
            return fail(sc, lineno, "RAISE/CLEAR need bed, device and code");
        if (st->kind == BG_STEP_ACK && st->alarm_id == 0)
            return fail(sc, lineno, "ACK needs alarm=");
        sc->n++;
    }
    return 0;
}

int bg_scenario_load(const char *path, bg_scenario *sc) {
    FILE *fp = fopen(path, "r");
    int rc;
    if (fp == NULL) {
        memset(sc, 0, sizeof *sc);
        snprintf(sc->error, sizeof sc->error, "cannot open %s", path);
        return -1;
    }
    rc = bg_scenario_parse(fp, sc);
    fclose(fp);
    return rc;
}

static void advance(bg_engine *e, uint64_t to) {
    uint64_t d;
    while ((d = bg_next_deadline(e)) <= to) bg_tick(e, d);
}

int bg_scenario_run_offline(const bg_scenario *sc, FILE *out) {
    static bg_engine eng;
    bg_log log;
    char line[BG_LOG_LINE_MAX + 2];
    if (bg_log_open(&log, NULL, 0) != 0) return -1;
    bg_engine_init(&eng, &log);
    for (int i = 0; i < sc->n; i++) {
        const bg_step *st = &sc->steps[i];
        advance(&eng, st->t_ms);
        switch (st->kind) {
        case BG_STEP_RAISE:
            bg_raise(&eng, st->t_ms, st->bed, st->device, st->code, st->prio, st->value, NULL);
            break;
        case BG_STEP_CLEAR:
            bg_clear(&eng, st->t_ms, st->bed, st->device, st->code, NULL);
            break;
        case BG_STEP_ACK: {
            const bg_alarm *a = bg_find(&eng, st->alarm_id);
            uint32_t v = a ? a->version : 0;
            char h[BG_HASH16_LEN + 1] = "";
            if (a) snprintf(h, sizeof h, "%.16s", a->vhash);
            if (st->seen_stale && v > 0) v--;
            if (st->hash16[0]) snprintf(h, sizeof h, "%s", st->hash16);
            bg_ack(&eng, st->t_ms, st->alarm_id, v, h, st->who, st->role, st->note,
                   st->request_id);
            break;
        }
        case BG_STEP_END:
            break;
        }
    }
    rewind(log.fp);
    while (fgets(line, sizeof line, log.fp)) {
        bg_log_record r;
        if (bg_log_parse(line, &r) == 0)
            fprintf(out, "%" PRIu64 " %s %s\n", r.t_ms, r.type, r.payload);
    }
    bg_log_close(&log);
    return 0;
}
