import os, signal, socket, struct, subprocess, sys, time
T = os.path.dirname(os.path.abspath(__file__))
SOCK = os.path.join(T, "helper.sock"); SCRIPT = os.path.join(T, "fake-vpnc-script"); TMP = os.path.join(T, "tmp")
big = len(sys.argv) > 1
if os.path.exists(SOCK): os.unlink(SOCK)
env = dict(os.environ, OCG_TEST_SOCKET=SOCK, OCG_TEST_SCRIPT=SCRIPT)
h = subprocess.Popen([os.path.join(T, "ocg-helper"), "--daemon"], env=env)
while not os.path.exists(SOCK): time.sleep(0.02)
lib, vpnfd = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
if big: lib.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
c = subprocess.Popen([os.path.join(T, "ocg-tun-client")], env={"OCG_TEST_SOCKET": SOCK, "TMPDIR": TMP, "VPNFD": str(vpnfd.fileno()), "VPNPID": str(os.getpid()), "INTERNAL_IP4_ADDRESS": "10.0.0.1"}, pass_fds=[vpnfd.fileno()], start_new_session=True)
vpnfd.close(); time.sleep(0.5)
N = 20000; pkt = b"\x45" + bytes(1339); sent = got = 0; lib.setblocking(False); t0 = time.time()
while got < N and time.time() - t0 < 30:
    while sent < N and sent - got < 64:
        try: lib.send(pkt); sent += 1
        except (BlockingIOError, OSError): break
    try:
        while True: lib.recv(2000); got += 1
    except BlockingIOError: pass
dt = time.time() - t0
print("%s lib buffer: %d/%d packets in %.2fs = %.1f MB/s each way" % ("1MB" if big else "default 4KB", got, N, dt, got * 1340 / dt / 1e6))
os.killpg(c.pid, signal.SIGHUP); c.wait(); h.terminate(); h.wait()
