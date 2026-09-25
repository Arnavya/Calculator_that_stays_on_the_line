#!/usr/bin/env python3
"""Single-connection integration test for the HTTP/1.1 calculator.

The point of this test is the receive-buffer invariant, not the arithmetic. It
opens EXACTLY ONE TCP connection and drives every request over that socket, so
a server that closed after the first response, or that lost track of where one
request ended and the next began, fails here.

Responses are framed exclusively by Content-Length. If the server sends a
short/merged response, or closes the connection early, the read below raises a
Failure naming the request that was in flight.
"""

import os
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(ROOT, "build", "server")

HOST = "127.0.0.1"


class Failure(Exception):
    """A test assertion failed. Always carries what was expected and observed."""


_checks = 0
_failures = []


def check(condition, what, detail=""):
    global _checks
    _checks += 1
    if not condition:
        _failures.append((what, detail))
        print("  FAIL: %s%s" % (what, ("\n        " + detail) if detail else ""))
    return condition


def check_eq(actual, expected, what):
    return check(actual == expected, what, "expected %r, got %r" % (expected, actual))


class Server:
    """The server under test, listening on a kernel-assigned port."""

    def __init__(self):
        self.proc = subprocess.Popen(
            [SERVER, "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        banner = self.proc.stdout.readline().strip()
        if not banner.startswith("listening on "):
            err = self.proc.stderr.read()
            raise Failure("server did not start: %r (stderr: %s)" % (banner, err))
        self.port = int(banner.rsplit(":", 1)[1])

    def finish(self):
        out, err = self.proc.communicate(timeout=15)
        return out, err


class Connection:
    """One TCP connection, persisting for the whole test."""

    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=10)
        self.sock.settimeout(10)
        self.buffer = b""

    def send(self, raw):
        if isinstance(raw, str):
            raw = raw.encode()
        self.sock.sendall(raw)

    def _fill(self, want, label):
        """Reads until at least `want` bytes are buffered.

        Any EOF here is the failure mode this test exists to catch: the server
        closed the persistent connection while a response was outstanding.
        """
        while len(self.buffer) < want:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise Failure(
                    "timed out waiting for %d bytes of the response to %s "
                    "(buffered %d so far) -- server is not responding"
                    % (want, label, len(self.buffer))
                )
            if not chunk:
                raise Failure(
                    "server CLOSED the connection while the response to %s was "
                    "still incomplete (buffered %d of %d bytes). The connection "
                    "must stay open and the response must be framed by "
                    "Content-Length, not by EOF."
                    % (label, len(self.buffer), want)
                )
            self.buffer += chunk

    def read_response(self, label):
        """Reads exactly one response, framed by Content-Length."""
        self._fill(4, label)
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Failure(
                    "server CLOSED the connection before finishing the headers of "
                    "the response to %s" % label
                )
            self.buffer += chunk

        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        parts = lines[0].split(b" ")
        if len(parts) < 2 or not parts[0].startswith(b"HTTP/1.1"):
            raise Failure("malformed status line for %s: %r" % (label, lines[0]))
        status = int(parts[1])

        headers = {}
        for line in lines[1:]:
            key, sep, value = line.partition(b":")
            if not sep:
                raise Failure("malformed header line for %s: %r" % (label, line))
            headers[key.strip().lower()] = value.strip()

        if b"content-length" not in headers:
            raise Failure(
                "response to %s has no Content-Length; the body could not be "
                "delimited without relying on the connection closing" % label
            )
        length = int(headers[b"content-length"])
        self._fill(length, label)

        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return status, body, headers

    def receive(self, status, body, label):
        """Reads one already-pending response and checks it."""
        got_status, got_body, headers = self.read_response(label)
        check_eq(got_status, status, "%s -> status" % label)
        if body is not None:
            check_eq(got_body, body, "%s -> body" % label)
        check_eq(headers.get(b"content-type"), b"text/plain", "%s -> Content-Type" % label)
        return got_status, got_body

    def expect(self, raw, status, body, label):
        """Sends one request, then reads and checks its response."""
        self.send(raw)
        return self.receive(status, body, label)

    def assert_no_leftover_bytes(self, label):
        check_eq(self.buffer, b"", "%s -> no unparsed bytes left in the stream" % label)

    def assert_still_open(self, label):
        """Confirms the socket is alive without consuming a response.

        A short timeout with no data is the pass condition: the server is
        holding the connection open, waiting for the next request.
        """
        self.sock.settimeout(0.5)
        try:
            chunk = self.sock.recv(1)
            check(False, label, "server sent unsolicited data %r" % chunk)
            return False
        except socket.timeout:
            return True
        finally:
            self.sock.settimeout(10)

    def close(self):
        self.sock.close()


def get(target, host="localhost"):
    return "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n" % (target, host)


REQUIRED_MATRIX = [
    ("/add?a=2&b=3", 200, b"5"),
    ("/sub?a=10&b=4", 200, b"6"),
    ("/mul?a=6&b=7", 200, b"42"),
    ("/div?a=9&b=3", 200, b"3"),
    ("/div?a=1&b=0", 400, None),
    ("/pow?a=2&b=8", 404, None),
]


# The six requests from the required matrix, reused for the pipelining test.
PIPELINE_MATRIX = [
    ("/add?a=2&b=3", 200, b"5"),
    ("/sub?a=10&b=4", 200, b"6"),
    ("/mul?a=6&b=7", 200, b"42"),
    ("/div?a=9&b=3", 200, b"3"),
    ("/div?a=1&b=0", 400, None),
    ("/pow?a=2&b=8", 404, None),
]


def test_pipelining(conn):
    """All six requests written in one send(), with no waiting in between.

    This is the receive-buffer invariant. If the parser consumed the wrong
    number of bytes, merged two requests, or skipped one, the responses would
    come back out of order or not at all.
    """
    print("6. six pipelined requests, written in a single send()")
    conn.send("".join(get(target) for target, _, _ in PIPELINE_MATRIX))
    for target, status, body in PIPELINE_MATRIX:
        conn.receive(status, body, "pipelined GET %s" % target)
    conn.assert_no_leftover_bytes("after the six pipelined responses")
    check(conn.assert_still_open("connection open after pipelining"),
          "connection stays open after a pipelined batch")
    print("   6 responses in request order, 0 stray bytes     OK")


def test_pipelined_batch_of_50(conn):
    """A larger batch, to catch ordering bugs that six requests might hide."""
    print("7. fifty pipelined requests in a single send()")
    batch = [("/add?a=%d&b=%d" % (i, i), 200, str(2 * i).encode()) for i in range(50)]
    conn.send("".join(get(target) for target, _, _ in batch))
    for target, status, body in batch:
        conn.receive(status, body, "pipelined #%s" % target)
    conn.assert_no_leftover_bytes("after the 50 pipelined responses")
    print("   50 responses, correct order, no request skipped   OK")


def test_pipelined_with_body(conn):
    """A pipelined batch containing a request that carries a body.

    The strongest desync check available: the body is announced by
    Content-Length, and the request after it can only parse if exactly the
    right number of body bytes was consumed.
    """
    print("8. a pipelined batch containing a request with a body")
    conn.send(
        get("/add?a=1&b=1")
        + "POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        + get("/mul?a=6&b=7")
        + get("/sub?a=10&b=4")
    )
    conn.receive(200, b"2", "pipelined #1 GET /add?a=1&b=1")
    conn.receive(405, None, "pipelined #2 POST /x with a 5-byte body")
    conn.receive(200, b"42", "pipelined #3 GET /mul?a=6&b=7")
    conn.receive(200, b"6", "pipelined #4 GET /sub?a=10&b=4")
    conn.assert_no_leftover_bytes("after the batch containing a body")
    print("   body consumed exactly, later requests intact      OK")


def test_usable_after_pipelining(conn):
    print("9. the connection is reusable after every pipelined batch")
    conn.expect(get("/div?a=9&b=3"), 200, b"3", "GET /div?a=9&b=3 (after pipelining)")
    check(conn.assert_still_open("connection open at end of test"),
          "connection still open after pipelined batches")
    print("   further requests answered on the same socket       OK")


def test_required_matrix(conn):
    print("1. the six required requests, in order, on one connection")
    for target, status, body in REQUIRED_MATRIX:
        conn.expect(get(target), status, body, "GET %s" % target)
    conn.assert_no_leftover_bytes("after the six responses")
    print("   6 responses, correct statuses and bodies          OK")


def test_connection_survives(conn):
    print("2. the socket is still usable after those six responses")
    check(conn.assert_still_open("connection stays open after 6 responses"),
          "connection remains open after the 4xx/404 responses")
    conn.expect(get("/add?a=40&b=2"), 200, b"42", "GET /add?a=40&b=2 (reused socket)")
    print("   a 7th request on the SAME socket succeeds        OK")


def test_method_not_allowed(conn):
    print("3. POST /add -> 405")
    conn.expect("POST /add HTTP/1.1\r\nHost: localhost\r\n\r\n", 405, None, "POST /add")
    conn.assert_no_leftover_bytes("after the 405")
    print("   405, connection still open                       OK")


def test_missing_host(conn):
    print("4. GET /add with no Host -> 400")
    conn.expect("GET /add HTTP/1.1\r\n\r\n", 400, None, "GET /add (no Host)")
    conn.assert_no_leftover_bytes("after the 400")
    print("   400, connection still open                       OK")


def test_still_usable_at_the_end(conn):
    print("5. the connection is still usable after every error response")
    conn.expect(get("/mul?a=6&b=7"), 200, b"42", "GET /mul?a=6&b=7 (final)")
    check(conn.assert_still_open("connection open at end of test"),
          "connection still open after 405 and 400 responses")
    print("   final request answered, socket still open         OK")


def main():
    if not os.path.exists(SERVER):
        print("error: %s not built; run `make` first" % SERVER)
        return 2

    server = Server()
    conn = Connection(server.port)
    print("server on port %d, ONE tcp connection for the whole test\n" % server.port)

    try:
        test_required_matrix(conn)
        test_connection_survives(conn)
        test_method_not_allowed(conn)
        test_missing_host(conn)
        test_still_usable_at_the_end(conn)
        test_pipelining(conn)
        test_pipelined_batch_of_50(conn)
        test_pipelined_with_body(conn)
        test_usable_after_pipelining(conn)
    except Failure as failure:
        print("\nFATAL: %s" % failure)
        _failures.append(("fatal", str(failure)))
    finally:
        conn.close()
        out, err = server.finish()

    print("\nserver log:\n%s" % "\n".join("    " + l for l in out.strip().splitlines()))

    # One connection means the server accepted exactly one.
    check_eq(out.count("connection accepted"), 1, "server accepted exactly one connection")
    check_eq(err, "", "server wrote nothing to stderr")
    check("clean shutdown" in out, "server shut down cleanly")

    print("\n%d checks, %d failures" % (_checks, len(_failures)))
    if _failures:
        print("INTEGRATION TEST: FAIL")
        return 1
    print("INTEGRATION TEST: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
