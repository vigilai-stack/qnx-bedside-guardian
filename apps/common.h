#ifndef BG_APPS_COMMON_H
#define BG_APPS_COMMON_H

#include <stdio.h>
#include <string.h>

#include "bg/clock.h"
#include "bg/proto.h"
#include "bg/transport.h"

static inline void bg_msg_init(bg_msg *m, uint16_t type) {
    memset(m, 0, sizeof *m);
    m->type = type;
    m->magic = BG_MAGIC;
}

static inline int bg_call(bg_client *c, bg_msg *m, bg_reply *r) {
    m->sent_ns = bg_now_ns();
    return bg_client_call(c, m, r);
}

#endif
