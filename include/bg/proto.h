/*
 * Wire messages between devices/consoles and the guardian.
 *
 * Fixed-size structs, sent as one message and answered with one reply. On
 * QNX this is a single MsgSend()/MsgReceive()/MsgReply() round trip; the
 * POSIX transport carries the same bytes over a local socket.
 *
 * The first field is a 16-bit type below 0x100, so QNX resource-manager
 * messages (_IO_BASE and up, e.g. _IO_CONNECT from name_open()) can be told
 * apart from ours, following the pattern in the QNX name_attach() docs.
 */
#ifndef BG_PROTO_H
#define BG_PROTO_H

#include <stdint.h>

#include "bg/alarm.h"

#define BG_MAGIC 0x42474431u /* "BGD1" */
#define BG_SERVICE_NAME "bedside_guardian"

enum {
    BG_MSG_RAISE = 0x01,
    BG_MSG_CLEAR = 0x02,
    BG_MSG_ACK = 0x03,
    BG_MSG_GET = 0x04,    /* one alarm, as a console would display it */
    BG_MSG_STATUS = 0x05, /* summary of open alarms */
    BG_MSG_SHUTDOWN = 0x06
};

typedef struct {
    uint16_t type;
    uint16_t prio;
    uint32_t magic;
    uint32_t alarm_id;
    uint32_t version;
    int32_t value;
    uint16_t role;
    uint16_t reserved;
    uint64_t request_id;
    uint64_t sent_ns; /* sender's CLOCK_MONOTONIC, for latency measurement */
    char bed[BG_BED_LEN];
    char device[BG_DEV_LEN];
    char code[BG_CODE_LEN];
    char who[BG_WHO_LEN];
    char hash16[BG_HASH16_LEN + 1];
    char note[BG_NOTE_LEN];
} bg_msg;

#define BG_REPLY_TEXT 1536

typedef struct {
    int32_t status; /* bg_status */
    uint32_t alarm_id;
    uint32_t version;
    uint16_t state;
    uint16_t tier;
    char hash16[BG_HASH16_LEN + 1];
    char text[BG_REPLY_TEXT];
} bg_reply;

#endif
