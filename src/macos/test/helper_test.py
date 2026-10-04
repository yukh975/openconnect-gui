#!/usr/bin/env python3
"""End-to-end test of ocg-helper + ocg-tun-client built with -DOCG_HELPER_TEST.

Plays libopenconnect: a datagram socket pair, one end passed as VPNFD.
"""
import os, signal, socket, struct, subprocess, sys, time

T = os.path.dirname(os.path.abspath(__file__))
SOCK = os.path.join(T, "helper.sock")
SCRIPT = os.path.join(T, "fake-vpnc-script")
LOG = os.path.join(T, "script.log")
TMP = os.path.join(T, "tmp")

fails = 0
def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1

os.makedirs(TMP, exist_ok=True)
for f in (LOG, os.path.join(TMP, "vpnc.log"), SOCK):
    if os.path.exists(f):
        os.unlink(f)
with open(SCRIPT, "w") as f:
    f.write("#!/bin/sh\n{ echo \"=== $reason\"; env | sort; } >> '%s'\necho \"script output for $reason\"\n" % LOG)
os.chmod(SCRIPT, 0o755)

env = dict(os.environ, OCG_TEST_SOCKET=SOCK, OCG_TEST_SCRIPT=SCRIPT)
helper = subprocess.Popen([os.path.join(T, "ocg-helper"), "--daemon"], env=env)
for _ in range(50):
    if os.path.exists(SOCK):
        break
    time.sleep(0.05)

# Version request, as the GUI does.
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SOCK)
s.sendall(struct.pack("=IIII", 0x4f434748, 1, 1, 0))
magic, version, status, ln = struct.unpack("=IIII", s.recv(16, socket.MSG_WAITALL))
s.close()
check(magic == 0x4f434748 and version == 1 and status == 0, "version request")

lib, vpnfd = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
lib.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
client_env = {
    "OCG_TEST_SOCKET": SOCK,
    "TMPDIR": TMP,
    "VPNFD": str(vpnfd.fileno()),
    "VPNPID": str(os.getpid()),
    "VPNGATEWAY": "203.0.113.7",
    "INTERNAL_IP4_ADDRESS": "10.120.14.7",
    "INTERNAL_IP4_MTU": "1340",
    "INTERNAL_IP4_NETMASK": "255.255.255.255",
    "INTERNAL_IP4_DNS": "10.77.2.32 10.77.2.35",
    "CISCO_DEF_DOMAIN": "smf.local",
    "CISCO_SPLIT_INC": "1",
    "CISCO_SPLIT_INC_0_ADDR": "10.77.0.0",
    "CISCO_SPLIT_INC_0_MASK": "255.255.0.0",
    "CISCO_SPLIT_INC_0_MASKLEN": "16",
    "CISCO_BANNER": "Welcome\nsecond line",
    # must not reach the script
    "INTERNAL_IP4_NBNS": "1.1.1.1\nremove State:/Network/Global/IPv4",
    "CISCO_SPLIT_DNS": "a.example;touch /tmp/pwned",
    "CISCO_EVIL": "1",
    "INTERNAL_IP4_NETADDR": "$(id)",
    "LD_PRELOAD": "/tmp/x.dylib",
}
client = subprocess.Popen(["/bin/sh", "-c", "'%s' utun5" % os.path.join(T, "ocg-tun-client")],
                          env=client_env, pass_fds=[vpnfd.fileno()], start_new_session=True)
vpnfd.close()

vpnc_log = os.path.join(TMP, "vpnc.log")
for _ in range(100):
    if os.path.exists(vpnc_log) and "script output for connect" in open(vpnc_log).read():
        break
    time.sleep(0.05)
text = open(vpnc_log).read() if os.path.exists(vpnc_log) else ""
check("tunnel interface utun99" in text, "client got the interface")
check("script output for connect" in text, "script output reaches $TMPDIR/vpnc.log")

script_env = open(LOG).read() if os.path.exists(LOG) else ""
connect_env = script_env.split("=== connect")[-1].split("===")[0]
check("reason=connect" in connect_env, "connect: reason")
check("TUNDEV=utun99" in connect_env, "connect: TUNDEV from the helper")
check("VPNPID=%d" % os.getpid() not in connect_env and "VPNPID=" in connect_env, "connect: VPNPID set by the helper")
check("PATH=/usr/bin:/bin:/usr/sbin:/sbin" in connect_env, "connect: fixed PATH")
for good in ("VPNGATEWAY=203.0.113.7", "INTERNAL_IP4_ADDRESS=10.120.14.7", "INTERNAL_IP4_DNS=10.77.2.32 10.77.2.35",
             "CISCO_SPLIT_INC_0_MASKLEN=16", "CISCO_DEF_DOMAIN=smf.local", "CISCO_BANNER=Welcome"):
    check(good in connect_env, "connect: passes " + good.split("=")[0])
for bad in ("INTERNAL_IP4_NBNS", "CISCO_SPLIT_DNS", "CISCO_EVIL", "INTERNAL_IP4_NETADDR", "LD_PRELOAD", "TMPDIR", "OCG_TEST"):
    check(bad not in connect_env, "connect: drops " + bad)

# Packets both ways through the fake utun (which echoes them).
N = 3000
sent = 0
received = 0
deadline = time.time() + 20
lib.setblocking(False)
pkts = []
for i in range(N):
    first = b"\x45" if i % 2 == 0 else b"\x60"
    pkts.append(first + struct.pack("!I", i) + bytes(1300 - 5))
while (sent < N or received < N) and time.time() < deadline:
    if sent < N:
        try:
            lib.send(pkts[sent])
            sent += 1
        except BlockingIOError:
            pass
        except OSError as e:
            if e.errno == 55:  # ENOBUFS: libopenconnect would retry later
                pass
            else:
                raise
    try:
        data = lib.recv(70000)
        if data[:1] in (b"\x45", b"\x60") and len(data) == 1300:
            received += 1
    except BlockingIOError:
        time.sleep(0.0005 if sent >= N else 0)
check(sent == N, "sent %d packets" % sent)
check(received == N, "got back %d of %d packets" % (received, N))

# Disconnect: libopenconnect sends SIGHUP to the script's process group.
os.killpg(client.pid, signal.SIGHUP)
client.wait(timeout=5)
for _ in range(100):
    if "=== disconnect" in open(LOG).read():
        break
    time.sleep(0.05)
script_env = open(LOG).read()
disc_env = script_env.split("=== disconnect")[-1] if "=== disconnect" in script_env else ""
check("reason=disconnect" in disc_env, "disconnect script ran")
check("TUNDEV=utun99" in disc_env and "INTERNAL_IP4_ADDRESS=10.120.14.7" in disc_env, "disconnect: same environment")
pid_c = [l for l in connect_env.splitlines() if l.startswith("VPNPID=")]
pid_d = [l for l in disc_env.splitlines() if l.startswith("VPNPID=")]
check(pid_c and pid_c == pid_d, "disconnect: same VPNPID as connect")

helper.send_signal(signal.SIGTERM)
helper.wait(timeout=5)
print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
