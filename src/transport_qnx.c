/*
 * QNX native message-passing transport.
 *
 * Server: name_attach() registers "bedside_guardian" in the path namespace
 * (/dev/name/local/bedside_guardian) and gives us a channel. Each request is
 * a synchronous MsgSend() from a device or console; we MsgReceive() it,
 * hand it to the engine and MsgReply() with the result. The sender stays
 * blocked until the reply, so a device knows its alarm was taken in, and
 * the receiving thread runs at the sender's priority (QNX priority
 * inheritance on message passing), so a HIGH alarm sent from a
 * high-priority thread is served at that priority.
 *
 * Escalation deadlines are kept by bounding each MsgReceive() with
 * TimerTimeout() on CLOCK_MONOTONIC.
 *
 * Status: written against the QNX SDP 8.0 documentation for name_attach(),
 * MsgReceive(), MsgReply(), TimerTimeout() and name_open(). It has NOT yet
 * been compiled with qcc in this repo's CI (the SDP needs a license
 * download). See docs/QNX.md.
 */
#include "bg/transport.h"

#if defined(__QNXNTO__) && !defined(BG_TRANSPORT_POSIX)

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <time.h>

struct bg_server {
    name_attach_t *attach;
};

struct bg_client {
    int coid;
};

typedef union {
    uint16_t type;
    struct _pulse pulse;
    bg_msg msg;
} recv_buf_t;

const char *bg_transport_name(void) { return "qnx-native-message-passing"; }

int bg_server_open(bg_server **out, const char *name) {
    bg_server *s = calloc(1, sizeof *s);
    if (s == NULL) return -1;
    s->attach = name_attach(NULL, name, 0);
    if (s->attach == NULL) {
        free(s);
        return -1;
    }
    *out = s;
    return 0;
}

int bg_server_receive(bg_server *s, bg_msg *m, int timeout_ms, bg_rcvid *rcvid) {
    recv_buf_t buf;
    for (;;) {
        rcvid_t id;
        if (timeout_ms >= 0) {
            uint64_t ns = (uint64_t)timeout_ms * 1000000ull;
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
        }
        id = MsgReceive(s->attach->chid, &buf, sizeof buf, NULL);
        if (id == -1) {
            if (errno == ETIMEDOUT) return 0;
            return -1;
        }
        if (id == 0) {
            /* Pulse. name_attach() channels get disconnect notifications. */
            switch (buf.pulse.code) {
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(buf.pulse.scoid);
                break;
            case _PULSE_CODE_UNBLOCK:
            default:
                break;
            }
            continue;
        }
        if (buf.type == _IO_CONNECT) {
            /* name_open() sends this; accept the connection. */
            MsgReply(id, EOK, NULL, 0);
            continue;
        }
        if (buf.type > _IO_BASE && buf.type <= _IO_MAX) {
            MsgError(id, ENOSYS);
            continue;
        }
        memcpy(m, &buf.msg, sizeof *m);
        *rcvid = (bg_rcvid)id;
        return 1;
    }
}

int bg_server_reply(bg_server *s, bg_rcvid rcvid, const bg_reply *r) {
    (void)s;
    return MsgReply((rcvid_t)rcvid, EOK, r, sizeof *r) == -1 ? -1 : 0;
}

void bg_server_close(bg_server *s) {
    if (s == NULL) return;
    name_detach(s->attach, 0);
    free(s);
}

int bg_client_open(bg_client **out, const char *name) {
    bg_client *c = calloc(1, sizeof *c);
    if (c == NULL) return -1;
    c->coid = name_open(name, 0);
    if (c->coid == -1) {
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

int bg_client_call(bg_client *c, const bg_msg *m, bg_reply *r) {
    return MsgSend(c->coid, m, sizeof *m, r, sizeof *r) == -1 ? -1 : 0;
}

void bg_client_close(bg_client *c) {
    if (c == NULL) return;
    name_close(c->coid);
    free(c);
}

#endif
