/*
 * bgd: the bedside guardian daemon.
 *
 * Receives alarm, clear and acknowledgement messages, runs the escalation
 * engine against CLOCK_MONOTONIC and writes the hash-chained event log.
 * On QNX it registers as "bedside_guardian" with name_attach(); on other
 * POSIX systems it listens on a Unix-domain socket.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bg/clock.h"
#include "bg/service.h"
#include "bg/transport.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(void) {
    fprintf(stderr,
            "usage: bgd [--name NAME] [--log FILE] [--incidents FILE] [--scale PCT]\n"
            "           [--rt PRIORITY] [--lock] [--sync] [--quiet]\n"
            "  --scale PCT   scale escalation windows (100 = demo policy, 10 = 10x faster)\n"
            "  --rt PRIO     run the receive loop SCHED_FIFO at PRIO\n"
            "  --lock        mlockall() so the loop never page-faults\n"
            "  --sync        fsync the event log after every event\n");
}

int main(int argc, char **argv) {
    const char *name = BG_SERVICE_NAME, *log_path = "bedside-guardian.log",
               *inc_path = "incidents.jsonl";
    int rt = 0, lock = 0, sync = 0, quiet = 0;
    uint32_t scale = 100;
    static bg_service svc;
    bg_server *srv = NULL;
    struct sigaction sa;
    uint64_t t0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--incidents") && i + 1 < argc) inc_path = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rt") && i + 1 < argc) rt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lock")) lock = 1;
        else if (!strcmp(argv[i], "--sync")) sync = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else {
            usage();
            return 2;
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: a signal interrupts the wait */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (lock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "bgd: mlockall failed (%s), continuing\n", strerror(errno));
    if (rt > 0) {
        struct sched_param sp = {.sched_priority = rt};
        int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (e != 0) fprintf(stderr, "bgd: SCHED_FIFO %d not allowed (%s), continuing\n", rt, strerror(e));
    }

    if (bg_service_init(&svc, log_path, inc_path, quiet ? NULL : stdout, sync) != 0) {
        fprintf(stderr, "bgd: cannot open log %s (or it failed verification)\n", log_path);
        return 1;
    }
    bg_engine_set_scale(&svc.eng, scale);
    if (bg_server_open(&srv, name) != 0) {
        fprintf(stderr, "bgd: cannot register service '%s': %s\n", name, strerror(errno));
        bg_service_close(&svc, 0);
        return 1;
    }
    printf("bgd: pid %d, transport %s, service '%s', log %s, scale %" PRIu32 "%%\n", (int)getpid(),
           bg_transport_name(), name, log_path, scale);
    printf("bgd: synthetic data only. Not a medical device.\n");
    fflush(stdout);

    t0 = bg_now_ns();
    while (!g_stop && !svc.shutdown_requested) {
        bg_msg m;
        bg_reply r;
        bg_rcvid rcvid = 0;
        uint64_t now_ms = (bg_now_ns() - t0) / 1000000ull;
        uint64_t nd = bg_next_deadline(&svc.eng);
        int timeout = 1000, rc;
        uint64_t rcv_ns;
        if (nd != UINT64_MAX) timeout = nd <= now_ms ? 0 : (int)(nd - now_ms < 1000 ? nd - now_ms : 1000);
        rc = bg_server_receive(srv, &m, timeout, &rcvid);
        rcv_ns = bg_now_ns();
        now_ms = (rcv_ns - t0) / 1000000ull;
        bg_tick(&svc.eng, now_ms);
        if (rc == 1) {
            uint64_t lat = (m.sent_ns && rcv_ns > m.sent_ns) ? rcv_ns - m.sent_ns : 0;
            bg_service_handle(&svc, now_ms, lat, &m, &r);
            bg_server_reply(srv, rcvid, &r);
        } else if (rc < 0 && errno != EINTR) {
            fprintf(stderr, "bgd: receive failed: %s\n", strerror(errno));
            break;
        }
    }

    {
        bg_latency lat;
        uint64_t now_ms = (bg_now_ns() - t0) / 1000000ull;
        bg_service_latency(&svc, &lat);
        printf("bgd: stopping. open=%d incidents=%" PRIu64 " events=%" PRIu64
               " raise latency p50=%.1fus p99=%.1fus max=%.1fus (n=%" PRIu64 ")\n",
               bg_open_count(&svc.eng), svc.eng.incidents, svc.log.seq + 1,
               (double)lat.p50_ns / 1e3, (double)lat.p99_ns / 1e3, (double)lat.max_ns / 1e3,
               lat.count);
        bg_server_close(srv);
        bg_service_close(&svc, now_ms);
    }
    return 0;
}
