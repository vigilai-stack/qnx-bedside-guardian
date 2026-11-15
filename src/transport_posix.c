/* POSIX transport: Unix-domain stream socket, one request per connection. */
#include "bg/transport.h"

#if !defined(__QNXNTO__) || defined(BG_TRANSPORT_POSIX)

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct bg_server {
    int fd;
    struct sockaddr_un addr;
};

struct bg_client {
    struct sockaddr_un addr;
};

const char *bg_transport_name(void) { return "posix-unix-socket"; }

static int make_addr(struct sockaddr_un *a, const char *name) {
    const char *dir = getenv("BG_SOCK_DIR");
    int n;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    n = snprintf(a->sun_path, sizeof a->sun_path, "%s/bg-%s.sock", dir && *dir ? dir : "/tmp",
                 name);
    return (n > 0 && (size_t)n < sizeof a->sun_path) ? 0 : -1;
}

static int io_all(int fd, void *buf, size_t len, int writing) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int bg_server_open(bg_server **out, const char *name) {
    bg_server *s = calloc(1, sizeof *s);
    if (s == NULL) return -1;
    if (make_addr(&s->addr, name) != 0) goto fail;
    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->fd < 0) goto fail;
    unlink(s->addr.sun_path);
    if (bind(s->fd, (struct sockaddr *)&s->addr, sizeof s->addr) != 0) goto fail_fd;
    if (listen(s->fd, 64) != 0) goto fail_fd;
    *out = s;
    return 0;
fail_fd:
    close(s->fd);
fail:
    free(s);
    return -1;
}

int bg_server_receive(bg_server *s, bg_msg *m, int timeout_ms, bg_rcvid *rcvid) {
    struct pollfd p = {.fd = s->fd, .events = POLLIN};
    struct timeval tv = {1, 0};
    int rc = poll(&p, 1, timeout_ms);
    int c;
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    c = accept(s->fd, NULL, NULL);
    if (c < 0) return errno == EINTR ? -1 : 0;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (io_all(c, m, sizeof *m, 0) != 0) {
        close(c);
        return 0;
    }
    *rcvid = c;
    return 1;
}

int bg_server_reply(bg_server *s, bg_rcvid rcvid, const bg_reply *r) {
    int c = (int)rcvid;
    int rc = io_all(c, (void *)r, sizeof *r, 1);
    (void)s;
    close(c);
    return rc;
}

void bg_server_close(bg_server *s) {
    if (s == NULL) return;
    close(s->fd);
    unlink(s->addr.sun_path);
    free(s);
}

int bg_client_open(bg_client **out, const char *name) {
    bg_client *c = calloc(1, sizeof *c);
    if (c == NULL) return -1;
    if (make_addr(&c->addr, name) != 0) {
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

int bg_client_call(bg_client *c, const bg_msg *m, bg_reply *r) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int rc = -1;
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&c->addr, sizeof c->addr) == 0 &&
        io_all(fd, (void *)m, sizeof *m, 1) == 0 && io_all(fd, r, sizeof *r, 0) == 0)
        rc = 0;
    close(fd);
    return rc;
}

void bg_client_close(bg_client *c) { free(c); }

#endif
