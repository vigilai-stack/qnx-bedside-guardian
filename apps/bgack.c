/*
 * bgack: the console side. Shows alarms and records a named person's
 * acknowledgement of exactly the version they were shown.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

static void usage(void) {
    fprintf(stderr,
            "usage: bgack [--name NAME] --status\n"
            "       bgack [--name NAME] --get ID\n"
            "       bgack [--name NAME] --ack ID --who NAME --role ROLE [--note TEXT]\n"
            "             [--version V --hash H16] [--request N]\n"
            "       bgack [--name NAME] --shutdown\n"
            "roles: BEDSIDE_RN, CHARGE_RN, RAPID_RESPONSE\n");
}

int main(int argc, char **argv) {
    const char *name = BG_SERVICE_NAME, *who = "", *note = "", *hash = NULL, *role_s = NULL;
    int op = 0;
    uint32_t id = 0, version = 0;
    int have_version = 0;
    uint64_t request = 0;
    bg_client *c = NULL;
    bg_msg m;
    bg_reply r;
    bg_role role = BG_ROLE_NONE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--status")) op = 's';
        else if (!strcmp(argv[i], "--shutdown")) op = 'x';
        else if (!strcmp(argv[i], "--get") && i + 1 < argc) op = 'g', id = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ack") && i + 1 < argc) op = 'a', id = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--who") && i + 1 < argc) who = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role_s = argv[++i];
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) note = argv[++i];
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hash = argv[++i];
        else if (!strcmp(argv[i], "--version") && i + 1 < argc)
            version = (uint32_t)atoi(argv[++i]), have_version = 1;
        else if (!strcmp(argv[i], "--request") && i + 1 < argc)
            request = strtoull(argv[++i], NULL, 10);
        else {
            usage();
            return 2;
        }
    }
    if (op == 0) {
        usage();
        return 2;
    }
    if (op == 'a' && (role_s == NULL || bg_role_parse(role_s, &role) != 0)) {
        fprintf(stderr, "bgack: --role must be BEDSIDE_RN, CHARGE_RN or RAPID_RESPONSE\n");
        return 2;
    }
    if (bg_client_open(&c, name) != 0) {
        fprintf(stderr, "bgack: cannot reach service '%s'\n", name);
        return 1;
    }

    if (op == 's' || op == 'x') {
        bg_msg_init(&m, op == 's' ? BG_MSG_STATUS : BG_MSG_SHUTDOWN);
        if (bg_call(c, &m, &r) != 0) return 1;
        fputs(r.text, stdout);
        if (op == 'x') putchar('\n');
        bg_client_close(c);
        return 0;
    }

    bg_msg_init(&m, BG_MSG_GET);
    m.alarm_id = id;
    if (bg_call(c, &m, &r) != 0) return 1;
    if (r.status != BG_OK) {
        printf("alarm %" PRIu32 ": %s\n", id, bg_status_name((bg_status)r.status));
        bg_client_close(c);
        return 1;
    }
    printf("showing: %s\n", r.text);
    if (op == 'g') {
        bg_client_close(c);
        return 0;
    }

    bg_msg_init(&m, BG_MSG_ACK);
    m.alarm_id = id;
    m.version = have_version ? version : r.version;
    snprintf(m.hash16, sizeof m.hash16, "%s", hash ? hash : r.hash16);
    snprintf(m.who, sizeof m.who, "%s", who);
    m.role = (uint16_t)role;
    snprintf(m.note, sizeof m.note, "%s", note);
    m.request_id = request ? request : ((bg_now_ns() << 8) ^ (uint64_t)getpid());
    if (bg_call(c, &m, &r) != 0) return 1;
    printf("ack: %s %s\n", bg_status_name((bg_status)r.status), r.text);
    bg_client_close(c);
    return r.status >= 0 ? 0 : 3;
}
