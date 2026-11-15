/*
 * The guardian service: turns wire messages into engine calls and replies,
 * prints the console view, writes incident records and keeps latency stats.
 * Transport-independent, so it is tested without any IPC.
 */
#ifndef BG_SERVICE_H
#define BG_SERVICE_H

#include <stdint.h>
#include <stdio.h>

#include "bg/alarm.h"
#include "bg/eventlog.h"
#include "bg/proto.h"

#define BG_LAT_SAMPLES 4096

typedef struct {
    bg_engine eng;
    bg_log log;
    FILE *incidents; /* JSON lines, may be NULL */
    FILE *console;   /* human-readable view, may be NULL */
    uint64_t lat_ns[BG_LAT_SAMPLES];
    uint64_t lat_count;
    uint64_t lat_max_ns;
    int shutdown_requested;
} bg_service;

typedef struct {
    uint64_t count, p50_ns, p99_ns, max_ns;
} bg_latency;

int bg_service_init(bg_service *s, const char *log_path, const char *incidents_path,
                    FILE *console, int sync);
/* now_ms: engine clock. latency_ns: receive time minus sender's sent_ns
 * (0 if unknown). */
void bg_service_handle(bg_service *s, uint64_t now_ms, uint64_t latency_ns, const bg_msg *m,
                       bg_reply *r);
void bg_service_latency(const bg_service *s, bg_latency *out);
void bg_service_close(bg_service *s, uint64_t now_ms);

/* JSON string escaping (used for incident records and the report). */
void bg_json_escape(const char *in, char *out, size_t cap);

#endif
