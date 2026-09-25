#!/usr/bin/env python3
"""Edge-case and error-path tests for the HTTP/1.1 calculator.

These drive the real server over real sockets, where parser_test.cpp and
edge_test.cpp cannot reach: partial requests that arrive over time, bodies that
show up late, and the cases where the server must answer 400 and then close.

The server accepts ONE connection and exits, so every scenario that ends the
connection needs its own server process. Scenarios that keep the connection
open are grouped onto a single connection, the way a real client would.

Responses are framed exclusively by Content-Length, so a response that arrives
short is reported as a failure rather than being mistaken for a closed
connection.
"""

import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "server")

HOST = "127.0.0.1"


class Failure(Exception):
    """A test assertion failed. Always carries what was expected and observed."""


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
    """A server process listening on a kernel-assigned port."""

    def __init__(self):
        self.proc = subprocess.Popen(
            [SERVER, "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        banner = self.proc.stdout.readline().strip()
        if not banner.startswith("listening on "):
            raise Failure("server did not start: %r" % banner)
        self.port = int(banner.rsplit(":", 1)[1])

    def finish(self):
        out, err = self.proc.communicate(timeout=30)
        return out, err


class Conn:
    """One TCP connection, with Content-Length-based response framing."""

    def __init__(self, port, timeout=10):
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def send(self, raw):
        if isinstance(raw, str):
            raw = raw.encode()
        self.sock.sendall(raw)

    def read_response(self, label, timeout=10):
        """Reads one response, framing it by Content-Length only."""
        self.sock.settimeout(timeout)
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Failure(
                    "server CLOSED the connection while the response to %s was still "
                    "incomplete (buffered %d bytes)" % (label, len(self.buf))
                )
            self.buf += chunk
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        status = int(lines[0].split(b" ")[1])
        headers = {}
        for line in lines[1:]:
            if b": " in line:
                k, v = line.split(b": ", 1)
                headers[k.lower()] = v
        if b"content-length" not in headers:
            raise Failure("response to %s has no Content-Length" % label)
        want = int(headers[b"content-length"])
        while len(rest) < want:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Failure(
                    "server CLOSED the connection mid-body for %s (wanted %d bytes, "
                    "had %d)" % (label, want, len(rest))
                )
            rest += chunk
        self.buf = rest[want:]
        return status, rest[:want], headers

    def expect(self, raw, status, body, label):
        self.send(raw)
        got_status, got_body, headers = self.read_response(label)
        check_eq(got_status, status, "%s -> status" % label)
        if body is not None:
            check_eq(got_body, body, "%s -> body" % label)
        return headers

    def assert_no_data(self, seconds, label):
        """Requires the server to stay silent: no early response."""
        self.sock.settimeout(seconds)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if data:
            raise Failure("server answered %s early: %r" % (label, data[:120]))

    def assert_closed_by_server(self, label, timeout=10):
        """Requires the server to close, and to have closed cleanly (EOF).

        ECONNRESET counts as closed. When the server closes while the client
        still has unread bytes queued, the kernel cannot send a FIN and sends
        an RST instead -- that is normal TCP behaviour, not a server fault. The
        response is verified complete by Content-Length before this is called.
        """
        self.sock.settimeout(timeout)
        try:
            while True:
                data = self.sock.recv(65536)
                if not data:
                    return
                self.buf += data
        except socket.timeout:
            raise Failure("server did not close the connection for %s" % label)
        except ConnectionResetError:
            return

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def request(path, host="localhost", extra=""):
    return "GET %s HTTP/1.1\r\nHost: %s\r\n%s\r\n" % (path, host, extra)


def run_isolated(name, body, expect_close_logged=True):
    """Runs one scenario against its own server and checks it shut down cleanly."""
    server = Server()
    conn = Conn(server.port)
    try:
        body(conn)
    except Failure as failure:
        check(False, "%s: no failure" % name, str(failure))
    finally:
        conn.close()
        out, err = server.finish()

    check_eq(err, "", "%s: server wrote nothing to stderr" % name)
    check_eq(server.proc.returncode, 0, "%s: server exited cleanly" % name)
    check("clean shutdown" in out, "%s: server shut down cleanly" % name)
    if expect_close_logged:
        # The server reports why it stopped differently depending on the path:
        # a lost-framing error closes after the response, an unusable head is
        # rejected up front, and a client that hangs up is logged as such.
        check(any(marker in out for marker in
                  ("client closed connection", "closing after response",
                   "rejecting request")),
              "%s: server logged why it stopped" % name,
              "log was: %s" % out.strip().replace("\n", " | "))
    return out


# --------------------------------------------------------------------------
# Scenarios that keep the connection open share one connection.
# --------------------------------------------------------------------------

def test_keepalive_errors_on_one_connection(conn):
    print("1. malformed input that does NOT lose framing, on one connection")
    # Each of these has a known head boundary, so the server must answer 400 and
    # keep serving. If any of them closed the connection, the next read fails.
    conn.expect("GET\r\nHost: h\r\n\r\n", 400, None, "truncated request line")
    conn.expect("GET / HTTP/1.0\r\nHost: h\r\n\r\n", 400, None, "HTTP/1.0 rejected")
    conn.expect("GET / HTTP/2.0\r\nHost: h\r\n\r\n", 400, None, "HTTP/2 rejected")
    conn.expect("GET /add HTTP/1.1\r\n\r\n", 400, None, "missing Host")
    conn.expect("GET /add HTTP/1.1\r\nBadHeader\r\n\r\n", 400, None, "malformed header")
    conn.expect("GET /add HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, None, "duplicate Host")
    conn.expect(request("/add"), 400, None, "missing parameters")
    conn.expect(request("/add?a=x&b=3"), 400, b"Bad Request: invalid a", "non-numeric a")
    conn.expect(request("/add?a=2&b="), 400, b"Bad Request: invalid b", "empty b")
    conn.expect(request("/add?a=9223372036854775808&b=1"), 400, None, "a out of range")
    conn.expect(request("/div?a=1&b=0"), 400, None, "division by zero")
    conn.expect(request("/pow?a=2&b=8"), 404, None, "unsupported route")
    conn.expect(request("/add/extra?a=1&b=2"), 404, None, "route with a path suffix")
    conn.expect("POST /add HTTP/1.1\r\nHost: h\r\n\r\n", 405, None, "unsupported method")
    conn.expect("DELETE /add?a=1&b=2 HTTP/1.1\r\nHost: h\r\n\r\n", 405, None, "DELETE")
    conn.expect("HEAD /add?a=1&b=2 HTTP/1.1\r\nHost: h\r\n\r\n", 405, None, "HEAD")
    print("   15 error responses, connection survived every one       OK")

    # And the connection is still fully usable afterwards.
    conn.expect(request("/add?a=2&b=3"), 200, b"5", "after 15 error responses")
    print("   still serving correctly afterwards                     OK")


def test_trickled_request(conn):
    print("2. a request trickled in across pauses")
    # A request line split mid-token, with a real pause in the middle. The
    # server must not answer until the head is delimited.
    conn.send("GET /add?a=2")
    conn.assert_no_data(0.7, "after a partial request line")
    conn.send("&b=3 HTTP/1.1\r\nHost: h\r\n")
    conn.assert_no_data(0.7, "after the headers but before the blank line")
    conn.send("\r\n")
    status, body, _ = conn.read_response("the completed trickled request")
    check_eq(status, 200, "trickled request -> status")
    check_eq(body, b"5", "trickled request -> body")
    print("   answered only once complete, correct result             OK")


def test_slow_body(conn):
    print("3. Content-Length body that arrives late")
    # A body announced in the head but sent much later. Answering before the
    # body arrives, or miscounting it, would corrupt every later request.
    conn.send("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n")
    conn.assert_no_data(0.7, "after the head of a body-bearing request")
    conn.send("hel")
    conn.assert_no_data(0.7, "after a partial body")
    conn.send("lo")
    status, body, _ = conn.read_response("the slow-body request")
    check_eq(status, 405, "body-bearing POST -> status")
    check_eq(body, b"Method Not Allowed", "body-bearing POST -> body")
    print("   waited for the whole body, then answered 405            OK")

    # The connection must still be in sync afterwards.
    conn.expect(request("/mul?a=6&b=7"), 200, b"42", "after a late body")
    print("   framing intact afterwards                               OK")


def test_expect_100_continue_over_the_wire(conn):
    print("4. Expect: 100-continue over a real socket")
    # The server does not implement 100-continue and does not reject the header.
    # What must not happen is a framing violation: the body must be consumed
    # exactly, and a request pipelined behind it must survive.
    conn.send("POST /x HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
              "Content-Length: 5\r\n\r\n")
    conn.assert_no_data(0.7, "after an Expect head with no body")
    conn.send("hello" + request("/add?a=40&b=2"))
    status, body, _ = conn.read_response("the Expect request")
    check_eq(status, 405, "Expect request -> status")
    status2, body2, _ = conn.read_response("the pipelined request behind it")
    check_eq(status2, 200, "pipelined request behind Expect -> status")
    check_eq(body2, b"42", "pipelined request behind Expect -> body")
    print("   body consumed exactly, next request intact              OK")


# --------------------------------------------------------------------------
# Scenarios that end the connection need their own server.
# --------------------------------------------------------------------------

def scenario_incomplete_then_stop(conn):
    print("5. an incomplete request, then the client stops")
    conn.send("GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n")  # no final CRLF
    conn.assert_no_data(0.7, "an incomplete request must not be answered")
    conn.send("X-Filler: " + "y" * 200 + "\r\n")  # still no terminator
    conn.assert_no_data(0.7, "still incomplete after more header bytes")
    # Now the client goes away without ever finishing the request.
    print("   never answered, client then closed                     OK")


def scenario_oversized_head(conn):
    print("6. a head that never terminates")
    # No CRLFCRLF, so this can never become a valid head. The server must stop
    # buffering, answer 400, and close.
    conn.send("GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\nX-Big: ")
    try:
        conn.send("z" * (70 * 1024))
    except (BrokenPipeError, ConnectionResetError):
        pass  # the server may hang up mid-write; that is also acceptable
    status, body, headers = conn.read_response("the oversized head")
    check_eq(status, 400, "oversized head -> status")
    check_eq(headers.get(b"connection"), b"close", "oversized head -> Connection: close")
    conn.assert_closed_by_server("the oversized head")
    print("   400 + Connection: close, full response then close       OK")


def scenario_oversized_body(conn):
    print("7. a Content-Length beyond the body cap")
    conn.send("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 2000000\r\n\r\n")
    status, body, headers = conn.read_response("the oversized body")
    check_eq(status, 400, "oversized body -> status")
    check_eq(body, b"Bad Request: invalid Content-Length", "oversized body -> body")
    check_eq(headers.get(b"connection"), b"close", "oversized body -> Connection: close")
    conn.assert_closed_by_server("the oversized body")
    print("   400 + Connection: close, body fully written              OK")


def make_bad_content_length_scenario(label, value):
    """One unusable Content-Length per server: each of these closes the
    connection, so they cannot share one."""

    def scenario(conn):
        print("8. a Content-Length that cannot be trusted (%s)" % label)
        conn.send("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: %s\r\n\r\n" % value)
        status, body, headers = conn.read_response("Content-Length: %s" % label)
        check_eq(status, 400, "Content-Length %s -> status" % label)
        check_eq(body, b"Bad Request: invalid Content-Length", "Content-Length %s -> body" % label)
        check_eq(headers.get(b"connection"), b"close", "Content-Length %s -> close" % label)
        conn.assert_closed_by_server("Content-Length: %s" % label)
        print("   400 + Connection: close, body fully written          OK")

    return scenario


def scenario_transfer_encoding(conn):
    print("9. Transfer-Encoding, which is not supported")
    conn.send("POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
              "5\r\nhello\r\n0\r\n\r\n")
    status, body, headers = conn.read_response("the chunked request")
    check_eq(status, 400, "chunked -> status")
    check_eq(body, b"Bad Request: Transfer-Encoding is not supported", "chunked -> body")
    check_eq(headers.get(b"connection"), b"close", "chunked -> Connection: close")
    conn.assert_closed_by_server("the chunked request")
    print("   400 + close, chunked framing refused                    OK")


def main():
    if not os.path.exists(SERVER):
        print("error: %s not built; run `make` first" % SERVER)
        return 2

    # Group 1: everything that keeps the connection open, on ONE connection.
    server = Server()
    conn = Conn(server.port)
    print("server on port %d, one connection for the keep-alive cases\n" % server.port)
    try:
        test_keepalive_errors_on_one_connection(conn)
        test_trickled_request(conn)
        test_slow_body(conn)
        test_expect_100_continue_over_the_wire(conn)
    except Failure as failure:
        print("\nFATAL: %s" % failure)
        _failures.append(("fatal", str(failure)))
    finally:
        conn.close()
        out, err = server.finish()
    check_eq(err, "", "keep-alive server wrote nothing to stderr")
    check_eq(server.proc.returncode, 0, "keep-alive server exited cleanly")

    # Group 2: each of these ends the connection, so each needs a fresh server.
    for name, fn, expect_close in [
        ("incomplete-then-stop", scenario_incomplete_then_stop, True),
        ("oversized-head", scenario_oversized_head, True),
        ("oversized-body", scenario_oversized_body, True),
        ("transfer-encoding", scenario_transfer_encoding, True),
    ]:
        print()
        run_isolated(name, fn, expect_close)

    # Each unusable Content-Length closes the connection, so each gets its own
    # server rather than sharing one.
    for label, value in [("non-numeric", "abc"), ("negative", "-1"),
                         ("comma-separated", "5, 5"), ("empty", ""),
                         ("overflowing", "99999999999999999999999")]:
        print()
        run_isolated("bad-content-length-" + label,
                     make_bad_content_length_scenario(label, value), True)

    print("\n%d checks, %d failures" % (_checks, len(_failures)))
    if _failures:
        print("EDGE-CASE TEST: FAIL")
        return 1
    print("EDGE-CASE TEST: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
