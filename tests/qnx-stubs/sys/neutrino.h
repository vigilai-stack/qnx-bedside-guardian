/* Type-check stub, NOT the real QNX header. Signatures per QNX SDP 8.0 docs. */
#ifndef BG_STUB_NEUTRINO_H
#define BG_STUB_NEUTRINO_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef long rcvid_t;
struct _msg_info;
struct _pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    union sigval value;
    int32_t scoid;
};
#define EOK 0
#define _NTO_TIMEOUT_RECEIVE (1 << 2)
#define _PULSE_CODE_UNBLOCK (-32)
#define _PULSE_CODE_DISCONNECT (-33)

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
int MsgError(rcvid_t rcvid, int error);
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
int TimerTimeout(clockid_t id, int flags, const struct sigevent *notify, const uint64_t *ntime,
                 uint64_t *otime);
int ConnectDetach(int coid);
#endif
