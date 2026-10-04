/*
 * Tun script of OpenConnect-GUI on macOS, see ocg-helper-protocol.h.
 *
 * libopenconnect starts it (openconnect_setup_tun_script()) with the
 * vpnc-script environment and VPNFD, one end of a datagram socket pair that
 * carries the tunnel's IP packets. It passes the environment to the
 * privileged helper, gets back a configured utun descriptor and then moves
 * packets between the two until the connection ends: libopenconnect sends
 * SIGHUP, the GUI exits, or either descriptor goes away. Closing the helper
 * connection makes the helper run vpnc-script with reason=disconnect.
 */
#include "ocg-helper-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern char** environ;

#define PKT_MAX 65536

static const char* const forwarded[] = {
    "INTERNAL_IP", "CISCO_", "VPNGATEWAY=", "IDLE_TIMEOUT=", "LOG_LEVEL=",
};

static int write_all(int fd, const void* buf, size_t len)
{
    const char* p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void* buf, size_t len)
{
    char* p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* The GUI shows $TMPDIR/vpnc.log in its log window. */
static void log_output(const char* text, size_t len)
{
    const char* tmp = getenv("TMPDIR");
    char path[1024];

    if (!len)
        return;
    fwrite(text, 1, len, stderr);
    snprintf(path, sizeof(path), "%s/vpnc.log", tmp && *tmp ? tmp : "/tmp");
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        write_all(fd, text, len);
        close(fd);
    }
}

static void log_line(const char* text)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "ocg-tun-client: %s\n", text);
    log_output(buf, n > 0 ? (size_t)n : 0);
}

static int connect_helper(void)
{
    struct sockaddr_un addr = { 0 };
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sock < 0)
        return -1;
    addr.sun_family = AF_UNIX;
#ifdef OCG_HELPER_TEST
    strlcpy(addr.sun_path, getenv("OCG_TEST_SOCKET"), sizeof(addr.sun_path));
#else
    strlcpy(addr.sun_path, OCG_HELPER_SOCKET, sizeof(addr.sun_path));
#endif
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFD, FD_CLOEXEC);
    return sock;
}

/* Sends the environment, returns the utun descriptor or -1. */
static int request_tunnel(int sock, const char* ifname)
{
    static char payload[OCG_MAX_PAYLOAD];
    size_t len = 0;

    if (ifname && *ifname) {
        int n = snprintf(payload, sizeof(payload), "OCG_IFNAME=%s", ifname);
        if (n > 0 && (size_t)n < 64)
            len = (size_t)n + 1;
    }

    for (char** e = environ; *e; e++) {
        bool wanted = false;
        for (size_t i = 0; i < sizeof(forwarded) / sizeof(forwarded[0]); i++)
            wanted |= !strncmp(*e, forwarded[i], strlen(forwarded[i]));
        size_t n = strlen(*e) + 1;
        if (!wanted || len + n > sizeof(payload))
            continue;
        memcpy(payload + len, *e, n);
        len += n;
    }

    struct ocg_msg_hdr hdr = { OCG_MAGIC, OCG_HELPER_VERSION, OCG_OP_CONNECT, (uint32_t)len };
    if (write_all(sock, &hdr, sizeof(hdr)) < 0 || write_all(sock, payload, len) < 0) {
        log_line("cannot talk to the helper");
        return -1;
    }

    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    struct iovec iov = { &hdr, sizeof(hdr) };
    struct msghdr msg = { 0 };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);
    ssize_t got;
    do {
        got = recvmsg(sock, &msg, MSG_WAITALL);
    } while (got < 0 && errno == EINTR);
    if (got != (ssize_t)sizeof(hdr) || hdr.magic != OCG_MAGIC || hdr.len > OCG_MAX_PAYLOAD) {
        log_line("no valid reply from the helper");
        return -1;
    }

    int tun = -1;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(&tun, CMSG_DATA(cmsg), sizeof(int));
    }

    char* reply = malloc(hdr.len + 1);
    if (!reply || read_all(sock, reply, hdr.len) < 0) {
        log_line("truncated reply from the helper");
        return -1;
    }
    reply[hdr.len] = '\0';

    if (hdr.code != 0) {
        log_output(reply, hdr.len);
        log_output("\n", 1);
        if (tun >= 0)
            close(tun);
        return -1;
    }
    /* Interface name, NUL, vpnc-script output. */
    size_t name_len = strnlen(reply, hdr.len);
    char line[128];
    snprintf(line, sizeof(line), "tunnel interface %s", reply);
    log_line(line);
    if (name_len < hdr.len)
        log_output(reply + name_len + 1, hdr.len - name_len - 1);
    free(reply);
    return tun;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static bool fatal_socket_error(int err)
{
    return err == ENOTCONN || err == ECONNREFUSED || err == ECONNRESET || err == EPIPE || err == EBADF;
}

/* utun frames start with the address family in network byte order. */
static int relay(int vpnfd, int tun, int helper, pid_t gui)
{
    static unsigned char tun_buf[PKT_MAX + 4];
    static unsigned char vpn_buf[PKT_MAX + 4];
    size_t pending = 0; /* packet from utun not yet accepted by libopenconnect */
    int kq = kqueue();
    struct kevent ev[8];
    int n = 0;

    if (kq < 0)
        return 1;
    set_nonblock(vpnfd);
    set_nonblock(tun);
    /* Unix datagram sockets default to a few KB of buffer on macOS: room
     * for two or three packets only. */
    int size = 1 << 20;
    setsockopt(vpnfd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    size = PKT_MAX + 1024;
    setsockopt(vpnfd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));

    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    EV_SET(&ev[n++], vpnfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&ev[n++], tun, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&ev[n++], helper, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&ev[n++], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&ev[n++], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&ev[n++], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (gui > 0)
        EV_SET(&ev[n++], gui, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (kevent(kq, ev, n, NULL, 0, NULL) < 0)
        return 1;

    for (;;) {
        struct kevent got[8];
        int count = kevent(kq, NULL, 0, got, 8, NULL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        for (int i = 0; i < count; i++) {
            struct kevent* e = &got[i];

            if (e->filter == EVFILT_SIGNAL || e->filter == EVFILT_PROC)
                return 0;
            if (e->filter == EVFILT_READ && (int)e->ident == helper) {
                char c;
                if (read(helper, &c, 1) <= 0)
                    return 0; /* the helper went away */
                continue;
            }
            if (e->filter == EVFILT_TIMER || (e->filter == EVFILT_READ && (int)e->ident == tun)) {
                /* utun -> libopenconnect. When its socket is full keep the
                 * packet, stop reading utun (the kernel queues there) and
                 * retry shortly: dropping here would throttle TCP. */
                for (;;) {
                    if (!pending) {
                        ssize_t r = read(tun, tun_buf, sizeof(tun_buf));
                        if (r < 0 && (errno == EAGAIN || errno == EINTR))
                            break;
                        if (r <= 0)
                            return 0;
                        if (r <= 4)
                            continue;
                        pending = (size_t)r;
                    }
                    if (send(vpnfd, tun_buf + 4, pending - 4, 0) < 0) {
                        if (errno == ENOBUFS || errno == EAGAIN || errno == EINTR) {
                            struct kevent ch[2];
                            EV_SET(&ch[0], tun, EVFILT_READ, EV_DISABLE, 0, 0, NULL);
                            EV_SET(&ch[1], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS, 200, NULL);
                            kevent(kq, ch, 2, NULL, 0, NULL);
                            break;
                        }
                        if (fatal_socket_error(errno))
                            return 0;
                        /* Anything else: drop the packet. */
                    }
                    if (pending && e->filter == EVFILT_TIMER) {
                        struct kevent ch;
                        EV_SET(&ch, tun, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
                        kevent(kq, &ch, 1, NULL, 0, NULL);
                    }
                    pending = 0;
                }
                continue;
            }
            if (e->filter == EVFILT_READ && (int)e->ident == vpnfd) {
                /* libopenconnect -> utun */
                for (;;) {
                    ssize_t r = recv(vpnfd, vpn_buf + 4, PKT_MAX, 0);
                    if (r < 0 && (errno == EAGAIN || errno == EINTR))
                        break;
                    if (r == 0 || (r < 0 && fatal_socket_error(errno)))
                        return 0;
                    if (r < 0)
                        break;
                    uint32_t family;
                    switch (vpn_buf[4] >> 4) {
                    case 4:
                        family = htonl(AF_INET);
                        break;
                    case 6:
                        family = htonl(AF_INET6);
                        break;
                    default:
                        continue;
                    }
                    memcpy(vpn_buf, &family, 4);
                    /* A full utun queue drops the packet, like a NIC. */
                    write(tun, vpn_buf, (size_t)r + 4);
                }
            }
        }
    }
}

/* argv[1]: the interface name set in the profile, if any. */
int main(int argc, char** argv)
{
    const char* fd_env = getenv("VPNFD");
    const char* pid_env = getenv("VPNPID");

    if (!fd_env) {
        fprintf(stderr, "ocg-tun-client is started by OpenConnect-GUI, not by hand\n");
        return 2;
    }
    int vpnfd = atoi(fd_env);
    pid_t gui = pid_env ? (pid_t)atoi(pid_env) : 0;

    int helper = connect_helper();
    if (helper < 0) {
        log_line("the privileged helper is not running; reinstall it from OpenConnect-GUI");
        return 1;
    }
    int tun = request_tunnel(helper, argc > 1 ? argv[1] : NULL);
    if (tun < 0)
        return 1;

    int rc = relay(vpnfd, tun, helper, gui);
    /* Closing the descriptors removes the interface; closing the helper
     * connection makes it run vpnc-script with reason=disconnect. */
    close(tun);
    close(vpnfd);
    close(helper);
    return rc;
}
