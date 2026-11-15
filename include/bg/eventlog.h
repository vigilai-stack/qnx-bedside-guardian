/*
 * Append-only, hash-chained event log.
 *
 * One event per line:
 *   seq|t_ms|TYPE|payload|prev_hash|hash
 * where hash = SHA-256("seq|t_ms|TYPE|payload|prev_hash") in hex and the
 * first event's prev_hash is 64 zeros. Changing, removing or reordering any
 * line breaks the chain from that point on, and bg_log_verify() reports the
 * first broken line.
 */
#ifndef BG_EVENTLOG_H
#define BG_EVENTLOG_H

#include <stdint.h>
#include <stdio.h>

#include "bg/sha256.h"

#define BG_LOG_TYPE_MAX 24
#define BG_LOG_PAYLOAD_MAX 512
#define BG_LOG_LINE_MAX (BG_LOG_PAYLOAD_MAX + 256)

typedef struct {
    FILE *fp;
    uint64_t seq;
    char head[BG_SHA256_HEX_LEN + 1];
    int sync; /* fsync after every event when non-zero */
} bg_log;

typedef struct {
    uint64_t seq;
    uint64_t t_ms;
    char type[BG_LOG_TYPE_MAX];
    char payload[BG_LOG_PAYLOAD_MAX];
    char prev[BG_SHA256_HEX_LEN + 1];
    char hash[BG_SHA256_HEX_LEN + 1];
} bg_log_record;

typedef struct {
    uint64_t events;
    uint64_t first_bad_line; /* 0 when the chain is intact */
    char reason[96];
    char head[BG_SHA256_HEX_LEN + 1];
} bg_verify_result;

/* Opens (or creates) a log. An existing log is verified first and appended
 * to; returns -1 if it is already broken. Pass path NULL for an anonymous
 * temporary log (tests). */
int bg_log_open(bg_log *log, const char *path, int sync);
int bg_log_append(bg_log *log, uint64_t t_ms, const char *type, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void bg_log_close(bg_log *log);

/* Parse one line. Returns 0 on success. */
int bg_log_parse(const char *line, bg_log_record *rec);
/* Verify a whole stream from the start. Returns 0 if intact, 1 if broken. */
int bg_log_verify(FILE *fp, bg_verify_result *res);
/* Replace characters that would break the line format ('|', CR, LF). */
void bg_log_sanitize(char *s);

#endif
