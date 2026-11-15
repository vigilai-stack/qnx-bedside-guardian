/*
 * Request/reply transport. Two implementations with the same interface:
 *   transport_qnx.c   QNX native message passing (name_attach, MsgReceive,
 *                     MsgReply / name_open, MsgSend). Built when
 *                     __QNXNTO__ is defined and BG_TRANSPORT_POSIX is not.
 *   transport_posix.c Unix-domain stream socket, one request per connection.
 *                     Used on Linux and available on QNX for comparison.
 */
#ifndef BG_TRANSPORT_H
#define BG_TRANSPORT_H

#include "bg/proto.h"

typedef struct bg_server bg_server;
typedef struct bg_client bg_client;

/* Opaque handle for the request currently being served. */
typedef long bg_rcvid;

int bg_server_open(bg_server **out, const char *name);
/* Wait up to timeout_ms (-1 = forever). Returns 1 with a message, 0 on
 * timeout, -1 on error or interrupted wait. */
int bg_server_receive(bg_server *s, bg_msg *m, int timeout_ms, bg_rcvid *rcvid);
int bg_server_reply(bg_server *s, bg_rcvid rcvid, const bg_reply *r);
void bg_server_close(bg_server *s);

int bg_client_open(bg_client **out, const char *name);
/* Blocking request/reply. Returns 0 on success. */
int bg_client_call(bg_client *c, const bg_msg *m, bg_reply *r);
void bg_client_close(bg_client *c);

const char *bg_transport_name(void);

#endif
