#!/usr/bin/env python3
"""Graceful shutdown tests: SIGINT and SIGTERM.

The server must not die on a signal. It has to notice the signal while parked
in a blocking syscall, stop accepting, release both sockets, print what it
did, and exit 0.

Two blocking points matter and are covered separately:

  * accept()   -- signal arrives before any client has connected
  * recv()     -- signal arrives on an idle, already-accepted connection

A signal delivered while the process sits in a plain blocking call is the easy
case to get wrong, because the process is not looking at a flag: it is waiting
for a syscall to return. That is what the self-pipe in net::Shutdown is for.
"""

import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "server")

HOST = "127.0.0.1"
# A shutdown must be prompt. Generous enough not to be flaky, tight enough that
# a server which only notices on the next client byte would fail it.
SHUTDOWN_DEADLINE = 5.0


class Failure(Exception):
    pass


_checks = 0
_failures = []


def check(ok, what, detail=""):
    global _checks
    _checks += 1
    if not ok:
        _failures.append((what, detail))
        print("  FAIL: %s\n        %s" % (what, detail))
    return ok


def check_eq(actual, expected, what):
    return check(actual == expected, what, "expected %r, got %r" % (expected, actual))


class Server:
    def __init__(self):
        self.proc = subprocess.Popen(
            [SERVER, "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        self.banner = self.proc.stdout.readline().strip()
        if not self.banner.startswith("listening on "):
            raise Failure("server did not start: %r" % self.banner)
        self.port = int(self.banner.rsplit(":", 1)[1])

    def connect(self, timeout=10):
        sock = socket.create_connection((HOST, self.port), timeout=timeout)
        sock.settimeout(timeout)
        return sock

    def signal_and_wait(self, sig, label):
        """Sends `sig`, then requires a prompt, clean, zero-status exit."""
        start = time.time()
        self.proc.send_signal(sig)
        try:
            out, err = self.proc.communicate(timeout=SHUTDOWN_DEADLINE)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate()
            check(False, "%s: exits promptly" % label,
                  "still running %.1fs after the signal" % SHUTDOWN_DEADLINE)
            return "", ""
        elapsed = time.time() - start
        check(elapsed < SHUTDOWN_DEADLINE, "%s: exits promptly" % label,
              "took %.2fs" % elapsed)
        check_eq(self.proc.returncode, 0, "%s: exit status is 0" % label)
        check_eq(err, "", "%s: nothing on stderr" % label)
        check("clean shutdown" in out, "%s: reports a clean shutdown" % label,
              "log was: %s" % out.strip().replace("\n", " | "))
        return out, err


def read_one_response(sock, label):
    sock.settimeout(10)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise Failure("server closed while the response to %s was incomplete" % label)
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    length = None
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b": ", 1)[1])
    while len(rest) < length:
        rest += sock.recv(65536)
    return head.split(b"\r\n")[0], rest[:length]


def test_sigint_while_blocked_in_accept():
    print("1. SIGINT while blocked in accept(), before any client connects")
    server = Server()
    out, _ = server.signal_and_wait(signal.SIGINT, "SIGINT in accept")
    check(server.banner.startswith("listening on "),
          "SIGINT in accept: reached the listening state")
    check("connections accepted: 0" in out, "SIGINT in accept: reports 0 connections",
          "log was: %s" % out.strip().replace("\n", " | "))
    # Nothing was ever accepted, so no request lines should be reported.
    check("request #" not in out, "SIGINT in accept: no requests were processed")
    print("   exit 0, clean shutdown, nothing to clean up               OK")


def test_sigint_while_blocked_in_recv():
    print("2. SIGINT while blocked in recv() on an idle connection")
    server = Server()
    sock = server.connect()
    try:
        # Make sure the server is really parked in recv() and not spinning.
        time.sleep(0.3)
        out, _ = server.signal_and_wait(signal.SIGINT, "SIGINT in recv")
        check("connection accepted" in out, "SIGINT in recv: connection was accepted")
        check("shutdown requested while connected" in out,
              "SIGINT in recv: noticed the signal while connected",
              "log was: %s" % out.strip().replace("\n", " | "))
        check("connections accepted: 1" in out, "SIGINT in recv: reports 1 connection")
    finally:
        sock.close()
    print("   stopped mid-connection instead of hanging                 OK")


def test_sigterm_while_blocked_in_recv():
    print("3. SIGTERM while blocked in recv()")
    server = Server()
    sock = server.connect()
    try:
        time.sleep(0.3)
        out, _ = server.signal_and_wait(signal.SIGTERM, "SIGTERM in recv")
        check("shutdown requested while connected" in out,
              "SIGTERM in recv: noticed the signal while connected",
              "log was: %s" % out.strip().replace("\n", " | "))
    finally:
        sock.close()
    print("   same clean path as SIGINT                               OK")


def test_sigint_after_processing_requests():
    print("4. SIGINT after real traffic, and the stats summary")
    server = Server()
    sock = server.connect()
    try:
        for path, expected in [("/add?a=2&b=3", b"5"), ("/mul?a=6&b=7", b"42"),
                               ("/div?a=9&b=3", b"3"), ("/pow?a=2&b=8", None)]:
            sock.sendall(("GET %s HTTP/1.1\r\nHost: h\r\n\r\n" % path).encode())
            status, body = read_one_response(sock, path)
            if expected is None:
                check(b"404" in status, "%s -> 404" % path, "got %r" % status)
            else:
                check(b"200" in status, "%s -> 200" % path, "got %r" % status)
                check_eq(body, expected, "%s -> body" % path)
        # A request the HTTP layer refuses: this is what the malformed counter
        # counts. A 404 or a 405 is a well-formed request answered with an error,
        # so it would not be counted as malformed.
        sock.sendall(b"GET /add HTTP/1.1\r\n\r\n")
        status, _ = read_one_response(sock, "missing Host")
        check(b"400" in status, "missing Host -> 400", "got %r" % status)
        # A routing rejection: processed, but not malformed.
        sock.sendall(b"POST /add HTTP/1.1\r\nHost: h\r\n\r\n")
        status, _ = read_one_response(sock, "POST /add")
        check(b"405" in status, "POST /add -> 405", "got %r" % status)

        out, _ = server.signal_and_wait(signal.SIGINT, "SIGINT after traffic")
        check("connections accepted: 1" in out, "summary: 1 connection accepted")
        check("requests processed:   6" in out, "summary: 6 requests processed",
              "log was: %s" % out.strip().replace("\n", " | "))
        check("(1 malformed)" in out, "summary: 1 malformed request")
        check("responses sent:       6" in out, "summary: 6 responses sent")
    finally:
        sock.close()
    print("   counts match the traffic, exit 0                        OK")


def test_sockets_released_after_shutdown():
    print("5. both sockets are released, so the port is reusable")
    server = Server()
    port = server.port
    sock = server.connect()
    sock.sendall(b"GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n\r\n")
    read_one_response(sock, "warmup")
    server.signal_and_wait(signal.SIGINT, "SIGINT for port reuse")
    sock.close()

    # A fresh server on the same port proves the listening socket was released
    # rather than left to the kernel to reap at exit.
    try:
        again = subprocess.Popen([SERVER, str(port)], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        banner = again.stdout.readline().strip()
        check("listening on 127.0.0.1:%d" % port in banner,
              "listening socket released (port immediately reusable)",
              "got %r" % banner)
        again.send_signal(signal.SIGTERM)
        again.communicate(timeout=SHUTDOWN_DEADLINE)
    except OSError as exc:
        check(False, "listening socket released (port immediately reusable)", str(exc))

    # And nothing is still listening once the process is gone.
    try:
        stray = socket.create_connection((HOST, port), timeout=1)
        stray.close()
        check(False, "no listener left behind", "connected to %d after shutdown" % port)
    except (ConnectionRefusedError, socket.timeout, OSError):
        check(True, "no listener left behind")
    print("   port reusable immediately, no listener left             OK")


def test_sigpipe_still_ignored():
    print("6. a vanished client must not kill the process with SIGPIPE")
    # The shutdown handlers install sigaction() for SIGINT and SIGTERM only.
    # SIGPIPE must still be ignored, or a client that disappears mid-response
    # would terminate the process instead of producing EPIPE.
    #
    # This asserts the invariant that actually matters -- the process is never
    # killed by SIGPIPE -- rather than assuming how it ends. This server serves
    # one client, so a client that vanishes can legitimately make it exit
    # normally, and which of the two happens depends on whether recv() or the
    # response write loses the race. The EPIPE-on-write path itself is covered
    # by net_test and tests/net_test.cpp.
    server = Server()
    sock = server.connect()
    try:
        sock.sendall(b"GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n\r\n")
        read_one_response(sock, "before the abrupt close")
        # SO_LINGER with a zero timeout makes close() send RST instead of FIN.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        b"\x01\x00\x00\x00\x00\x00\x00\x00")
        sock.close()
        time.sleep(0.5)

        rc = server.proc.poll()
        check(rc != -signal.SIGPIPE, "process not killed by SIGPIPE",
              "terminated by signal %r" % (-rc if rc is not None and rc < 0 else rc))
        if rc is None:
            # Still running: it must still shut down cleanly on request.
            out, _ = server.signal_and_wait(signal.SIGINT, "SIGINT after RST")
            check("clean shutdown" in out, "still able to shut down cleanly")
        else:
            # Already gone, which is allowed for a one-connection server, but
            # only if it left on its own terms.
            check_eq(rc, 0, "exited cleanly after the client vanished")
    finally:
        try:
            sock.close()
        except OSError:
            pass
    print("   not killed by SIGPIPE, clean exit either way             OK")


def main():
    if not os.path.exists(SERVER):
        print("error: %s not built; run `make` first" % SERVER)
        return 2

    for fn in [test_sigint_while_blocked_in_accept,
               test_sigint_while_blocked_in_recv,
               test_sigterm_while_blocked_in_recv,
               test_sigint_after_processing_requests,
               test_sockets_released_after_shutdown,
               test_sigpipe_still_ignored]:
        print()
        try:
            fn()
        except Failure as failure:
            check(False, "%s: no failure" % fn.__name__, str(failure))

    print("\n%d checks, %d failures" % (_checks, len(_failures)))
    if _failures:
        print("SHUTDOWN TEST: FAIL")
        return 1
    print("SHUTDOWN TEST: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
