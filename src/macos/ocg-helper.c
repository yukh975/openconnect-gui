/*
 * Privileged helper of OpenConnect-GUI on macOS.
 *
 *   ocg-helper --daemon     run by launchd (socket activated) as root
 *   ocg-helper --install    run once by the GUI with administrator rights:
 *                           copies itself and vpnc-script out of the app
 *                           bundle into root-owned locations and loads the
 *                           launchd job
 *   ocg-helper --uninstall  unloads the job and removes the files
 *
 * The daemon creates a utun interface and runs its own copy of vpnc-script
 * for the console user, see ocg-helper-protocol.h. It never runs anything
 * but that script, and the script only gets the environment variables that
 * vpnc-script knows, with values limited to the characters they can contain.
 */
#include "ocg-helper-protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <launch.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <os/log.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sys_domain.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

#define IDLE_EXIT_SECONDS 30
#define SCRIPT_TIMEOUT_SECONDS 60
#define SCRIPT_OUTPUT_MAX (64 * 1024)
#define MAX_ENV 512

static os_log_t logger;

/* OCG_HELPER_TEST builds a helper for tests run without root: it listens on
 * $OCG_TEST_SOCKET, runs $OCG_TEST_SCRIPT and hands out a fake utun that
 * echoes every packet back. Release builds never define it. */
#ifdef OCG_HELPER_TEST
#define SCRIPT_PATH (getenv("OCG_TEST_SCRIPT"))
#else
#define SCRIPT_PATH OCG_HELPER_SCRIPT
#endif

/* ------------------------------------------------------------------ */
/* I/O helpers                                                        */
/* ------------------------------------------------------------------ */

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

/* Sends the reply header and payload; the descriptor, if any, travels with
 * the header. */
static int send_reply(int sock, uint32_t status, const void* payload, uint32_t len, int fd)
{
    struct ocg_msg_hdr hdr = { OCG_MAGIC, OCG_HELPER_VERSION, status, len };
    struct iovec iov = { &hdr, sizeof(hdr) };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr msg = { 0 };

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&control, 0, sizeof(control));
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof(control.buf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    ssize_t n;
    do {
        n = sendmsg(sock, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(hdr))
        return -1;
    return len ? write_all(sock, payload, len) : 0;
}

static void send_error(int sock, int err, const char* text)
{
    os_log_error(logger, "%{public}s", text);
    send_reply(sock, (uint32_t)err, text, (uint32_t)strlen(text), -1);
}

/* ------------------------------------------------------------------ */
/* Environment filtering                                              */
/* ------------------------------------------------------------------ */

enum value_class {
    V_NONE,
    V_NUMBER,  /* digits */
    V_TOKENS,  /* addresses, masks, domains: a space separated list */
    V_LINE,    /* printable, single line */
    V_TEXT,    /* anything but NUL, e.g. the banner */
};

static const struct {
    const char* name;
    enum value_class cls;
} known_vars[] = {
    { "VPNGATEWAY", V_TOKENS },
    { "INTERNAL_IP4_ADDRESS", V_TOKENS },
    { "INTERNAL_IP4_MTU", V_NUMBER },
    { "INTERNAL_IP4_NETMASK", V_TOKENS },
    { "INTERNAL_IP4_NETMASKLEN", V_NUMBER },
    { "INTERNAL_IP4_NETADDR", V_TOKENS },
    { "INTERNAL_IP4_DNS", V_TOKENS },
    { "INTERNAL_IP4_NBNS", V_TOKENS },
    { "INTERNAL_IP6_ADDRESS", V_TOKENS },
    { "INTERNAL_IP6_NETMASK", V_TOKENS },
    { "INTERNAL_IP6_DNS", V_TOKENS },
    { "CISCO_DEF_DOMAIN", V_TOKENS },
    { "CISCO_SPLIT_DNS", V_TOKENS },
    { "CISCO_SPLIT_INC", V_NUMBER },
    { "CISCO_SPLIT_EXC", V_NUMBER },
    { "CISCO_IPV6_SPLIT_INC", V_NUMBER },
    { "CISCO_IPV6_SPLIT_EXC", V_NUMBER },
    { "CISCO_BANNER", V_TEXT },
    { "CISCO_CSTP_OPTIONS", V_LINE },
    { "CISCO_PROXY_PAC", V_LINE },
    { "IDLE_TIMEOUT", V_NUMBER },
    { "LOG_LEVEL", V_NUMBER },
};

/* CISCO_SPLIT_INC_3_ADDR and friends. */
static enum value_class split_var_class(const char* name, size_t len)
{
    static const char* const prefixes[] = {
        "CISCO_SPLIT_INC_", "CISCO_SPLIT_EXC_",
        "CISCO_IPV6_SPLIT_INC_", "CISCO_IPV6_SPLIT_EXC_",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t plen = strlen(prefixes[i]);
        if (len <= plen || strncmp(name, prefixes[i], plen) != 0)
            continue;
        const char* p = name + plen;
        const char* end = name + len;
        if (!isdigit((unsigned char)*p))
            return V_NONE;
        while (p < end && isdigit((unsigned char)*p))
            p++;
        if (p >= end || *p != '_')
            return V_NONE;
        p++;
        size_t rest = (size_t)(end - p);
        bool v6 = (i >= 2);
        if (rest == 4 && !strncmp(p, "ADDR", 4))
            return V_TOKENS;
        if (rest == 7 && !strncmp(p, "MASKLEN", 7))
            return V_NUMBER;
        if (!v6 && rest == 4 && !strncmp(p, "MASK", 4))
            return V_TOKENS;
        if (!v6 && ((rest == 8 && !strncmp(p, "PROTOCOL", 8)) || (rest == 5 && !strncmp(p, "SPORT", 5)) || (rest == 5 && !strncmp(p, "DPORT", 5))))
            return V_NUMBER;
        return V_NONE;
    }
    return V_NONE;
}

static enum value_class var_class(const char* name, size_t len)
{
    for (size_t i = 0; i < sizeof(known_vars) / sizeof(known_vars[0]); i++) {
        if (strlen(known_vars[i].name) == len && !strncmp(name, known_vars[i].name, len))
            return known_vars[i].cls;
    }
    return split_var_class(name, len);
}

static bool value_ok(enum value_class cls, const char* v)
{
    size_t len = strlen(v);

    if (len > (cls == V_TEXT ? 65536u : 4096u))
        return false;
    for (const unsigned char* p = (const unsigned char*)v; *p; p++) {
        switch (cls) {
        case V_NUMBER:
            if (!isdigit(*p))
                return false;
            break;
        case V_TOKENS:
            if (!isalnum(*p) && !strchr(" .,:/%_+-", *p))
                return false;
            break;
        case V_LINE:
            if (*p < 0x20 || *p == 0x7f)
                return false;
            break;
        case V_TEXT:
            break;
        case V_NONE:
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Session: utun + vpnc-script                                        */
/* ------------------------------------------------------------------ */

#ifdef OCG_HELPER_TEST
static int open_utun(int wanted_unit, char* ifname, size_t ifname_len)
{
    int sv[2];
    (void)wanted_unit;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
        return -1;
    /* A real utun queues in the kernel; give the fake one room too. */
    int size = 1 << 20;
    for (int i = 0; i < 2; i++) {
        setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        int snd = 70000;
        setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    }
    if (fork() == 0) {
        char buf[70000];
        close(sv[0]);
        for (;;) {
            ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
            if (n <= 0)
                _exit(0);
            while (send(sv[1], buf, (size_t)n, 0) < 0 && errno == ENOBUFS)
                usleep(100);
        }
    }
    close(sv[1]);
    strlcpy(ifname, "utun99", ifname_len);
    return sv[0];
}
#else
static int open_utun(int wanted_unit, char* ifname, size_t ifname_len)
{
    struct ctl_info info;
    int first = wanted_unit >= 0 ? wanted_unit : 0;
    int last = wanted_unit >= 0 ? wanted_unit : 254;

    for (int unit = first; unit <= last; unit++) {
        int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
        if (fd < 0)
            return -1;
        memset(&info, 0, sizeof(info));
        strlcpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));
        if (ioctl(fd, CTLIOCGINFO, &info) < 0) {
            close(fd);
            return -1;
        }
        struct sockaddr_ctl addr = { 0 };
        addr.sc_len = sizeof(addr);
        addr.sc_family = AF_SYSTEM;
        addr.ss_sysaddr = AF_SYS_CONTROL;
        addr.sc_id = info.ctl_id;
        addr.sc_unit = (uint32_t)unit + 1; /* utunN is unit N + 1 */
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            int err = errno;
            close(fd);
            if (err == EBUSY && wanted_unit < 0)
                continue;
            errno = err;
            return -1;
        }
        socklen_t len = (socklen_t)ifname_len;
        if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &len) < 0) {
            close(fd);
            return -1;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
    errno = EBUSY;
    return -1;
}
#endif

/* The script must be the root-owned copy made at install time. */
static bool script_trusted(void)
{
#ifdef OCG_HELPER_TEST
    return true;
#endif
    struct stat st;
    if (lstat(OCG_HELPER_SCRIPT, &st) < 0 || !S_ISREG(st.st_mode))
        return false;
    return st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

struct script_env {
    char* vars[MAX_ENV + 8];
    int count; /* filtered variables from the client */
};

/* Runs vpnc-script with the given reason; returns its exit status (or -1)
 * and appends its output to out. */
static int run_script(struct script_env* env, const char* reason, const char* ifname, char* out, size_t* out_len, size_t out_max)
{
    char reason_var[64], tundev_var[IFNAMSIZ + 8], pid_var[32];
    int pipefd[2];

    snprintf(reason_var, sizeof(reason_var), "reason=%s", reason);
    snprintf(tundev_var, sizeof(tundev_var), "TUNDEV=%s", ifname);
    /* vpnc-script names its state files after VPNPID: use ours, constant
     * for connect and disconnect and not under the client's control. */
    snprintf(pid_var, sizeof(pid_var), "VPNPID=%d", (int)getpid());

    int n = env->count;
    env->vars[n++] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
    env->vars[n++] = reason_var;
    env->vars[n++] = tundev_var;
    env->vars[n++] = pid_var;
    env->vars[n] = NULL;

    if (pipe(pipefd) < 0)
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* Close everything else in the child, e.g. the client socket. */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    char* argv[] = { "/bin/sh", SCRIPT_PATH, NULL };
    pid_t pid;
    int err = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, env->vars);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (err) {
        close(pipefd[0]);
        errno = err;
        return -1;
    }

    time_t deadline = time(NULL) + SCRIPT_TIMEOUT_SECONDS;
    char buf[4096];
    bool timed_out = false;
    for (;;) {
        int left = (int)(deadline - time(NULL));
        if (left <= 0) {
            timed_out = true;
            break;
        }
        struct pollfd p = { pipefd[0], POLLIN, 0 };
        int r = poll(&p, 1, left * 1000);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            timed_out = (r == 0);
            break;
        }
        ssize_t got = read(pipefd[0], buf, sizeof(buf));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        size_t take = (size_t)got;
        if (*out_len + take > out_max)
            take = out_max - *out_len;
        memcpy(out + *out_len, buf, take);
        *out_len += take;
    }
    close(pipefd[0]);
    if (timed_out) {
        os_log_error(logger, "vpnc-script (%{public}s) timed out", reason);
        kill(-pid, SIGKILL);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    os_log(logger, "vpnc-script %{public}s on %{public}s exited with %d", reason, ifname, rc);
    return rc;
}

static volatile sig_atomic_t terminate;

static void on_term(int sig)
{
    (void)sig;
    terminate = 1;
}

static int handle_connect(int sock, char* payload, uint32_t len)
{
    static struct script_env env;
    int wanted_unit = -1;

    env.count = 0;
    for (char* p = payload; p < payload + len; p += strlen(p) + 1) {
        char* eq = strchr(p, '=');
        if (!eq)
            continue;
        size_t name_len = (size_t)(eq - p);
        if (name_len == 10 && !strncmp(p, "OCG_IFNAME", 10)) {
            const char* v = eq + 1;
            if (!strncmp(v, "utun", 4) && v[4] && strlen(v) <= 7 && strspn(v + 4, "0123456789") == strlen(v + 4))
                wanted_unit = atoi(v + 4);
            else if (*v)
                os_log(logger, "ignoring interface name '%{public}s'", v);
            continue;
        }
        enum value_class cls = var_class(p, name_len);
        if (cls == V_NONE) {
            os_log(logger, "dropping variable %{public}.*s", (int)name_len, p);
            continue;
        }
        if (!value_ok(cls, eq + 1)) {
            os_log(logger, "dropping variable %{public}.*s: unexpected value", (int)name_len, p);
            continue;
        }
        if (env.count >= MAX_ENV) {
            send_error(sock, E2BIG, "Too many variables");
            return 1;
        }
        env.vars[env.count++] = p;
    }

    if (!script_trusted()) {
        send_error(sock, EPERM, "vpnc-script of the helper is missing or not owned by root");
        return 1;
    }

    char ifname[IFNAMSIZ] = "";
    int tun = open_utun(wanted_unit, ifname, sizeof(ifname));
    if (tun < 0) {
        char text[128];
        snprintf(text, sizeof(text), "Cannot create the utun interface: %s", strerror(errno));
        send_error(sock, (uint32_t)errno, text);
        return 1;
    }
    os_log(logger, "created %{public}s", ifname);

    /* Reply payload: interface name, NUL, script output. */
    static char out[IFNAMSIZ + SCRIPT_OUTPUT_MAX];
    size_t out_len = strlen(ifname) + 1;
    memcpy(out, ifname, out_len);
    run_script(&env, "connect", ifname, out, &out_len, sizeof(out));

    if (send_reply(sock, 0, out, (uint32_t)out_len, tun) < 0) {
        os_log_error(logger, "cannot reply to the client: %{public}s", strerror(errno));
        close(tun);
        out_len = 0;
        run_script(&env, "disconnect", ifname, out, &out_len, sizeof(out));
        return 1;
    }
    /* From now on the interface lives as long as the client holds it. */
    close(tun);

    /* Wait until the client goes away (or launchd stops us). */
    struct sigaction sa = { 0 };
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    char buf[256];
    while (!terminate) {
        ssize_t n = read(sock, buf, sizeof(buf));
        if (n == 0 || (n < 0 && errno != EINTR))
            break;
    }

    out_len = 0;
    run_script(&env, "disconnect", ifname, out, &out_len, sizeof(out));
    os_log(logger, "session on %{public}s finished", ifname);
    return 0;
}

static bool uid_allowed(uid_t uid)
{
#ifdef OCG_HELPER_TEST
    return uid == getuid();
#endif
    if (uid == 0)
        return true;
    uid_t console = (uid_t)-1;
    CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &console, NULL);
    if (name)
        CFRelease(name);
    return name && uid == console;
}

static int handle_client(int sock)
{
    uid_t uid;
    gid_t gid;
    struct ocg_msg_hdr hdr;
    struct timeval tv = { 10, 0 };

    signal(SIGPIPE, SIG_IGN);
    if (getpeereid(sock, &uid, &gid) < 0)
        return 1;
    if (!uid_allowed(uid)) {
        os_log_error(logger, "refusing uid %d: not the console user", (int)uid);
        send_error(sock, EPERM, "Only the user logged in at the console may use the helper");
        return 1;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (read_all(sock, &hdr, sizeof(hdr)) < 0 || hdr.magic != OCG_MAGIC)
        return 1;

    if (hdr.code == OCG_OP_VERSION) {
        send_reply(sock, 0, NULL, 0, -1);
        return 0;
    }
    if (hdr.version != OCG_HELPER_VERSION) {
        send_error(sock, EPROTO, "The helper and OpenConnect-GUI versions differ");
        return 1;
    }
    if (hdr.code != OCG_OP_CONNECT || hdr.len == 0 || hdr.len > OCG_MAX_PAYLOAD) {
        send_error(sock, EINVAL, "Invalid request");
        return 1;
    }

    char* payload = malloc(hdr.len + 1);
    if (!payload || read_all(sock, payload, hdr.len) < 0)
        return 1;
    payload[hdr.len] = '\0';

    /* The session lasts as long as the VPN connection. */
    tv.tv_sec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    os_log(logger, "connect request from uid %d", (int)uid);
    return handle_connect(sock, payload, hdr.len);
}

#define SESSION_FD 3

static char self_path[PATH_MAX];

static int spawn_session(int sock, pid_t* pid)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    char* argv[] = { self_path, "--session", NULL };

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, sock, SESSION_FD);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    int err = posix_spawn(pid, self_path, &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (err) {
        os_log_error(logger, "cannot start a session: %{public}s", strerror(err));
        return -1;
    }
    return 0;
}

static int run_daemon(void)
{
    int* fds = NULL;
    size_t count = 0;

#ifdef OCG_HELPER_TEST
    struct sockaddr_un addr = { 0 };
    int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, getenv("OCG_TEST_SOCKET"), sizeof(addr.sun_path));
    unlink(addr.sun_path);
    if (bind(test_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(test_fd, 8) < 0)
        return 1;
    fds = malloc(sizeof(int));
    fds[0] = test_fd;
    count = 1;
#else
    if (launch_activate_socket("Listener", &fds, &count) != 0 || count < 1) {
        os_log_error(logger, "no launchd socket; the helper must be started by launchd");
        return 1;
    }
#endif
    int listener = fds[0];
    free(fds);
    signal(SIGPIPE, SIG_IGN);

    int children = 0;
    time_t last_activity = time(NULL);
    for (;;) {
        pid_t pid;
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
            children--;

        struct pollfd p = { listener, POLLIN, 0 };
        int r = poll(&p, 1, 1000);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0) {
            int sock = accept(listener, NULL, NULL);
            if (sock < 0)
                continue;
            /* Each session is a fresh process: after a plain fork() the
             * system libraries (os_log, CoreFoundation) are not safe. */
            if (spawn_session(sock, &pid) == 0)
                children++;
            close(sock);
            last_activity = time(NULL);
        } else if (children <= 0 && time(NULL) - last_activity > IDLE_EXIT_SECONDS) {
            /* launchd starts us again on the next connection. Sessions are
             * our children; we must stay while they run, or launchd would
             * kill them together with us. */
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Installation                                                       */
/* ------------------------------------------------------------------ */

static int run_cmd(char* const argv[])
{
    pid_t pid;
    int status;
    posix_spawn_file_actions_t actions;

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    int err = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (err)
        return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Writes data to path atomically, owned by root. */
static int write_file(const char* path, const void* data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write_all(fd, data, len) < 0 || fchown(fd, 0, 0) < 0 || fchmod(fd, mode) < 0 || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int copy_file(const char* src, const char* dst, mode_t mode)
{
    int fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    char* data = malloc((size_t)st.st_size + 1);
    int rc = data ? read_all(fd, data, (size_t)st.st_size) : -1;
    close(fd);
    if (rc == 0)
        rc = write_file(dst, data, (size_t)st.st_size, mode);
    free(data);
    return rc;
}

static const char launchd_plist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Label</key>\n"
    "\t<string>" OCG_HELPER_LABEL "</string>\n"
    "\t<key>ProgramArguments</key>\n"
    "\t<array>\n"
    "\t\t<string>" OCG_HELPER_BIN "</string>\n"
    "\t\t<string>--daemon</string>\n"
    "\t</array>\n"
    "\t<key>Sockets</key>\n"
    "\t<dict>\n"
    "\t\t<key>Listener</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>SockPathName</key>\n"
    "\t\t\t<string>" OCG_HELPER_SOCKET "</string>\n"
    "\t\t\t<key>SockPathMode</key>\n"
    "\t\t\t<integer>438</integer>\n" /* 0666; access is checked per connection */
    "\t\t</dict>\n"
    "\t</dict>\n"
    "</dict>\n"
    "</plist>\n";

static void unload_job(void)
{
    char* argv[] = { "/bin/launchctl", "bootout", "system/" OCG_HELPER_LABEL, NULL };
    run_cmd(argv);
}

/* Settings saved while the GUI ran as root, for the GUI now running as the
 * user. Prefer the current domain; fall back to the one used up to v1.5.x. */
static void export_settings(void)
{
    static const CFStringRef domains[] = {
        CFSTR("com.openconnect-gui-team.OpenConnect-GUI"),
        CFSTR("io.github.openconnect.OpenConnect-GUI"),
    };

    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++) {
        CFArrayRef keys = CFPreferencesCopyKeyList(domains[i], kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        if (!keys)
            continue;
        if (CFArrayGetCount(keys) == 0) {
            CFRelease(keys);
            continue;
        }
        CFDictionaryRef values = CFPreferencesCopyMultiple(keys, domains[i], kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        CFRelease(keys);
        if (!values)
            continue;
        CFDataRef data = CFPropertyListCreateData(NULL, values, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        CFRelease(values);
        if (!data)
            continue;
        printf("%s %ld\n", OCG_INSTALL_SETTINGS, (long)CFDataGetLength(data));
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), stdout);
        printf("\n");
        CFRelease(data);
        return;
    }
}

/* Running as root, the GUI created its log directories owned by root in the
 * user's home. Hand them back, without following symbolic links anywhere. */
static void chown_tree(int dirfd, uid_t uid, gid_t gid, int depth)
{
    if (depth > 4)
        return;
    int fd = dup(dirfd);
    DIR* dir = fd >= 0 ? fdopendir(fd) : NULL;
    if (!dir) {
        if (fd >= 0)
            close(fd);
        return;
    }
    struct dirent* de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;
        if (st.st_uid == 0)
            fchownat(dirfd, de->d_name, uid, gid, AT_SYMLINK_NOFOLLOW);
        if (S_ISDIR(st.st_mode)) {
            int sub = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub >= 0) {
                chown_tree(sub, uid, gid, depth + 1);
                close(sub);
            }
        }
    }
    closedir(dir);
}

static void fix_log_ownership(uid_t uid)
{
    struct passwd* pw = getpwuid(uid);
    if (!pw || !pw->pw_dir || uid == 0)
        return;
    const char* parts[] = { "Library", "Application Support", "OpenConnect-GUI Team" };
    int fd = open(pw->pw_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (size_t i = 0; fd >= 0 && i < sizeof(parts) / sizeof(parts[0]); i++) {
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next >= 0 && i == sizeof(parts) / sizeof(parts[0]) - 1) {
            struct stat st;
            if (fstat(next, &st) == 0 && st.st_uid == 0)
                fchown(next, uid, pw->pw_gid);
        }
        close(fd);
        fd = next;
    }
    if (fd >= 0) {
        chown_tree(fd, uid, pw->pw_gid, 0);
        close(fd);
    }
}

static int install_fail(const char* what)
{
    printf("%s %s: %s\n", OCG_INSTALL_ERR, what, strerror(errno));
    return 1;
}

static int run_install(bool with_settings)
{
    char self[PATH_MAX], real[PATH_MAX], script[PATH_MAX];
    uint32_t size = sizeof(self);

    if (geteuid() != 0) {
        errno = EPERM;
        return install_fail("must run as root");
    }
    if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, real))
        return install_fail("cannot locate the helper");
    snprintf(script, sizeof(script), "%s/../Resources/vpnc-script", dirname(real));
    /* dirname() may modify its argument; resolve again for the copy. */
    size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, real))
        return install_fail("cannot locate the helper");

    unload_job();
    if (mkdir("/Library/PrivilegedHelperTools", 0755) < 0 && errno != EEXIST)
        return install_fail("/Library/PrivilegedHelperTools");
    if (copy_file(real, OCG_HELPER_BIN, 0755) < 0)
        return install_fail(OCG_HELPER_BIN);
    if (copy_file(script, OCG_HELPER_SCRIPT, 0755) < 0)
        return install_fail(OCG_HELPER_SCRIPT);
    if (write_file(OCG_HELPER_PLIST, launchd_plist, sizeof(launchd_plist) - 1, 0644) < 0)
        return install_fail(OCG_HELPER_PLIST);

    /* bootout returns before the old job is gone; bootstrap fails meanwhile. */
    char* argv[] = { "/bin/launchctl", "bootstrap", "system", OCG_HELPER_PLIST, NULL };
    int rc = -1;
    for (int attempt = 0; attempt < 10 && rc != 0; attempt++) {
        if (attempt)
            sleep(1);
        rc = run_cmd(argv);
    }
    if (rc != 0) {
        errno = EIO;
        return install_fail("launchctl bootstrap");
    }

    /* Started with administrator rights by the GUI, the real uid is the
     * user's; should it be root, take the console user. */
    uid_t uid = getuid();
    if (uid == 0) {
        CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
        if (name)
            CFRelease(name);
        else
            uid = 0;
    }
    fix_log_ownership(uid);
    if (with_settings)
        export_settings();
    printf("%s\n", OCG_INSTALL_OK);
    fflush(stdout);
    return 0;
}

static int run_uninstall(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "must run as root\n");
        return 1;
    }
    unload_job();
    unlink(OCG_HELPER_PLIST);
    unlink(OCG_HELPER_BIN);
    unlink(OCG_HELPER_SCRIPT);
    unlink(OCG_HELPER_SOCKET);
    printf("removed\n");
    return 0;
}

int main(int argc, char** argv)
{
    logger = os_log_create(OCG_HELPER_LABEL, "helper");

    if (argc >= 2 && !strcmp(argv[1], "--daemon")) {
        uint32_t size = sizeof(self_path);
        if (_NSGetExecutablePath(self_path, &size) != 0)
            return 1;
        return run_daemon();
    }
    if (argc >= 2 && !strcmp(argv[1], "--session"))
        return handle_client(SESSION_FD);
    if (argc >= 2 && !strcmp(argv[1], "--install"))
        return run_install(argc >= 3 && !strcmp(argv[2], "--export-settings"));
    if (argc >= 2 && !strcmp(argv[1], "--uninstall"))
        return run_uninstall();
    if (argc >= 2 && !strcmp(argv[1], "--version")) {
        printf("%d\n", OCG_HELPER_VERSION);
        return 0;
    }
    fprintf(stderr, "usage: %s --install [--export-settings] | --uninstall | --version\n", argv[0]);
    return 2;
}
