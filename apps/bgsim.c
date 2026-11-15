/*
 * bgsim: replays a scenario of synthetic bedside devices and staff
 * against a running bgd, or measures round-trip latency (--bench).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bg/scenario.h"
#include "common.h"

static void sleep_until_ns(uint64_t target) {
    struct timespec ts = {(time_t)(target / 1000000000ull), (long)(target % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int bench(bg_client *c, int n) {
    uint64_t *rt = calloc((size_t)n, sizeof *rt);
    int fails = 0;
    if (rt == NULL) return 1;
    for (int i = 0; i < n; i++) {
        bg_msg m;
        bg_reply r;
        uint64_t t;
        bg_msg_init(&m, BG_MSG_RAISE);
        m.prio = BG_PRIO_LOW;
        snprintf(m.bed, sizeof m.bed, "bench");
        snprintf(m.device, sizeof m.device, "bench-%d", i % 64);
        snprintf(m.code, sizeof m.code, "BENCH");
        t = bg_now_ns();
        if (bg_call(c, &m, &r) != 0 || r.status < 0) fails++;
        rt[i] = bg_now_ns() - t;
        bg_msg_init(&m, BG_MSG_CLEAR);
        snprintf(m.bed, sizeof m.bed, "bench");
        snprintf(m.device, sizeof m.device, "bench-%d", i % 64);
        snprintf(m.code, sizeof m.code, "BENCH");
        bg_call(c, &m, &r);
    }
    qsort(rt, (size_t)n, sizeof *rt, cmp_u64);
    printf("bench: %d RAISE round trips over %s, failures=%d\n", n, bg_transport_name(), fails);
    printf("bench: p50=%.1fus p99=%.1fus max=%.1fus\n", (double)rt[(n - 1) / 2] / 1e3,
           (double)rt[(n * 99 + 99) / 100 - 1] / 1e3, (double)rt[n - 1] / 1e3);
    free(rt);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *name = BG_SERVICE_NAME, *path = NULL;
    uint32_t speed = 100;
    int bench_n = 0;
    static bg_scenario sc;
    bg_client *c = NULL;
    uint64_t t0;
    int bad = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_n = atoi(argv[++i]);
        else if (argv[i][0] != '-') path = argv[i];
        else {
            fprintf(stderr, "usage: bgsim [--name NAME] [--speed PCT] SCENARIO | --bench N\n");
            return 2;
        }
    }
    if (bg_client_open(&c, name) != 0) {
        fprintf(stderr, "bgsim: cannot reach service '%s' (is bgd running?)\n", name);
        return 1;
    }
    if (bench_n > 0) {
        int rc = bench(c, bench_n);
        bg_client_close(c);
        return rc;
    }
    if (path == NULL || bg_scenario_load(path, &sc) != 0) {
        fprintf(stderr, "bgsim: %s\n", path ? sc.error : "no scenario given");
        bg_client_close(c);
        return 2;
    }
    if (speed == 0) speed = 100;

    t0 = bg_now_ns();
    for (int i = 0; i < sc.n; i++) {
        const bg_step *st = &sc.steps[i];
        bg_msg m;
        bg_reply r;
        sleep_until_ns(t0 + st->t_ms * 1000000ull * speed / 100u);
        switch (st->kind) {
        case BG_STEP_RAISE:
        case BG_STEP_CLEAR:
            bg_msg_init(&m, st->kind == BG_STEP_RAISE ? BG_MSG_RAISE : BG_MSG_CLEAR);
            m.prio = (uint16_t)st->prio;
            m.value = st->value;
            snprintf(m.bed, sizeof m.bed, "%s", st->bed);
            snprintf(m.device, sizeof m.device, "%s", st->device);
            snprintf(m.code, sizeof m.code, "%s", st->code);
            break;
        case BG_STEP_ACK: {
            /* Look at the alarm first, like a nurse reading the console. */
            bg_msg g;
            bg_reply gr;
            bg_msg_init(&g, BG_MSG_GET);
            g.alarm_id = st->alarm_id;
            if (bg_call(c, &g, &gr) != 0) {
                fprintf(stderr, "bgsim: lost the service\n");
                bg_client_close(c);
                return 1;
            }
            bg_msg_init(&m, BG_MSG_ACK);
            m.alarm_id = st->alarm_id;
            m.version = st->seen_stale && gr.version > 0 ? gr.version - 1 : gr.version;
            snprintf(m.hash16, sizeof m.hash16, "%s", st->hash16[0] ? st->hash16 : gr.hash16);
            snprintf(m.who, sizeof m.who, "%s", st->who);
            m.role = (uint16_t)st->role;
            snprintf(m.note, sizeof m.note, "%s", st->note);
            m.request_id = st->request_id;
            break;
        }
        case BG_STEP_END:
            continue;
        }
        if (bg_call(c, &m, &r) != 0) {
            fprintf(stderr, "bgsim: lost the service\n");
            bad = 1;
            break;
        }
        printf("sim t=%6" PRIu64 "ms line %-3d -> %-13s %s\n", st->t_ms, st->line,
               bg_status_name((bg_status)r.status), r.text);
        fflush(stdout);
    }
    bg_client_close(c);
    return bad;
}
