#include "bg/eventlog.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char ZERO_HASH[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

void bg_log_sanitize(char *s) {
    for (; *s; s++) {
        if (*s == '|') *s = '/';
        else if (*s == '\n' || *s == '\r') *s = ' ';
    }
}

static void chain_hash(uint64_t seq, uint64_t t_ms, const char *type, const char *payload,
                       const char *prev, char out[BG_SHA256_HEX_LEN + 1]) {
    char head[64];
    uint8_t digest[BG_SHA256_LEN];
    bg_sha256_ctx c;
    int n = snprintf(head, sizeof head, "%" PRIu64 "|%" PRIu64 "|", seq, t_ms);
    bg_sha256_init(&c);
    bg_sha256_update(&c, head, (size_t)n);
    bg_sha256_update(&c, type, strlen(type));
    bg_sha256_update(&c, "|", 1);
    bg_sha256_update(&c, payload, strlen(payload));
    bg_sha256_update(&c, "|", 1);
    bg_sha256_update(&c, prev, strlen(prev));
    bg_sha256_final(&c, digest);
    bg_hex(digest, sizeof digest, out);
}

static int split_field(char **cur, char **field) {
    char *bar;
    if (*cur == NULL) return -1;
    *field = *cur;
    bar = strchr(*cur, '|');
    if (bar) {
        *bar = '\0';
        *cur = bar + 1;
    } else {
        *cur = NULL;
    }
    return 0;
}

int bg_log_parse(const char *line, bg_log_record *rec) {
    char buf[BG_LOG_LINE_MAX];
    char *cur = buf, *f[6];
    size_t len = strlen(line);
    char *end;
    if (len >= sizeof buf) return -1;
    memcpy(buf, line, len + 1);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    for (int i = 0; i < 6; i++) {
        if (split_field(&cur, &f[i]) != 0) return -1;
    }
    if (cur != NULL) return -1; /* too many fields */
    rec->seq = strtoull(f[0], &end, 10);
    if (*f[0] == '\0' || *end != '\0') return -1;
    rec->t_ms = strtoull(f[1], &end, 10);
    if (*f[1] == '\0' || *end != '\0') return -1;
    if (strlen(f[2]) >= sizeof rec->type || strlen(f[3]) >= sizeof rec->payload) return -1;
    if (strlen(f[4]) != BG_SHA256_HEX_LEN || strlen(f[5]) != BG_SHA256_HEX_LEN) return -1;
    strcpy(rec->type, f[2]);
    strcpy(rec->payload, f[3]);
    strcpy(rec->prev, f[4]);
    strcpy(rec->hash, f[5]);
    return 0;
}

int bg_log_verify(FILE *fp, bg_verify_result *res) {
    char line[BG_LOG_LINE_MAX + 2];
    char expect_prev[BG_SHA256_HEX_LEN + 1];
    uint64_t lineno = 0;
    memset(res, 0, sizeof *res);
    strcpy(expect_prev, ZERO_HASH);
    strcpy(res->head, ZERO_HASH);
    while (fgets(line, sizeof line, fp)) {
        bg_log_record r;
        char calc[BG_SHA256_HEX_LEN + 1];
        lineno++;
        if (bg_log_parse(line, &r) != 0) {
            res->first_bad_line = lineno;
            snprintf(res->reason, sizeof res->reason, "line %" PRIu64 " is malformed", lineno);
            return 1;
        }
        if (r.seq != lineno) {
            res->first_bad_line = lineno;
            snprintf(res->reason, sizeof res->reason,
                     "line %" PRIu64 " has seq %" PRIu64 " (missing or reordered event)", lineno,
                     r.seq);
            return 1;
        }
        if (strcmp(r.prev, expect_prev) != 0) {
            res->first_bad_line = lineno;
            snprintf(res->reason, sizeof res->reason,
                     "line %" PRIu64 " does not link to the line before it", lineno);
            return 1;
        }
        chain_hash(r.seq, r.t_ms, r.type, r.payload, r.prev, calc);
        if (strcmp(calc, r.hash) != 0) {
            res->first_bad_line = lineno;
            snprintf(res->reason, sizeof res->reason,
                     "line %" PRIu64 " was changed after it was written", lineno);
            return 1;
        }
        strcpy(expect_prev, r.hash);
        res->events = lineno;
        strcpy(res->head, r.hash);
    }
    return 0;
}

int bg_log_open(bg_log *log, const char *path, int sync) {
    bg_verify_result vr;
    memset(log, 0, sizeof *log);
    log->sync = sync;
    if (path == NULL) {
        log->fp = tmpfile();
    } else {
        log->fp = fopen(path, "a+");
    }
    if (log->fp == NULL) return -1;
    rewind(log->fp);
    if (bg_log_verify(log->fp, &vr) != 0) {
        fclose(log->fp);
        log->fp = NULL;
        return -1;
    }
    log->seq = vr.events;
    strcpy(log->head, vr.head);
    fseek(log->fp, 0, SEEK_END);
    return 0;
}

int bg_log_append(bg_log *log, uint64_t t_ms, const char *type, const char *fmt, ...) {
    char payload[BG_LOG_PAYLOAD_MAX];
    char t[BG_LOG_TYPE_MAX];
    char hash[BG_SHA256_HEX_LEN + 1];
    va_list ap;
    if (log->fp == NULL) return -1;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof payload, fmt, ap);
    va_end(ap);
    snprintf(t, sizeof t, "%s", type);
    bg_log_sanitize(payload);
    bg_log_sanitize(t);
    chain_hash(log->seq + 1, t_ms, t, payload, log->head, hash);
    if (fprintf(log->fp, "%" PRIu64 "|%" PRIu64 "|%s|%s|%s|%s\n", log->seq + 1, t_ms, t, payload,
                log->head, hash) < 0)
        return -1;
    if (fflush(log->fp) != 0) return -1;
    if (log->sync) fsync(fileno(log->fp));
    log->seq++;
    strcpy(log->head, hash);
    return 0;
}

void bg_log_close(bg_log *log) {
    if (log->fp) fclose(log->fp);
    log->fp = NULL;
}
