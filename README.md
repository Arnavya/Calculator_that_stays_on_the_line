# HTTP/1.1 Calculator Server

A small HTTP/1.1 server written in C++17 using raw POSIX sockets — no HTTP
library, no framework. It serves four arithmetic routes over a **persistent
connection** and correctly handles pipelined requests.

The point of the project is the request-framing logic: treating TCP as a byte
stream and extracting exactly one HTTP request at a time from a persistent
receive buffer.

## What it does

- Accepts **one** TCP connection and serves it until the client disconnects.
- Answers many requests on that single connection (keep-alive).
- Supports HTTP/1.1 request **pipelining**: several requests written in one
  `send()` are answered in order.
- Parses request lines, headers, and `Content-Length`-framed bodies itself.
- Adds, subtracts, multiplies, and divides two signed 64-bit integers.

## Requirements

- A C++17 compiler (developed with Apple Clang 17)
- GNU Make
- Python 3 (standard library only — for the integration test client)

No third-party dependencies.

## Build

```sh
make
```

Produces `build/server`. Object files and dependency files go in `build/`;
`make clean` removes the directory.

## Run

```sh
./build/server 8080        # bind 127.0.0.1:8080
./build/server 0           # bind an arbitrary free port (useful for tests)
```

The port argument is optional and defaults to `8080`. The server prints the
**actual** bound port on its first line, which matters when `0` is requested:

```
listening on 127.0.0.1:8080
```

It then accepts a single connection and serves it:

```
connection accepted
request #1: GET /add?a=2&b=3 (head=46 body=0, 0 left)
...
client closed connection
served 3 requests (0 rejected) on one connection
clean shutdown
```

The `(head=… body=… N left)` fields on each request line are the framing
internals, described under [Request framing](#request-framing) below.

## Test

```sh
make test                   # unit + integration + edge-case tests
make unit-test              # C++ unit tests only
make integration-test       # Python socket-level integration test only
make edge-integration-test  # Python malformed-input / error-path test only

make sanitize          # full suite under AddressSanitizer + UndefinedBehaviorSanitizer
make harden            # full suite under libc++ container hardening
```

`make integration-test` runs `tests/integration_test.py`, which launches
`build/server 0` on an ephemeral port, reads the real port from its output, and
drives it over a real TCP socket. The test relies on `Content-Length` alone to
find response boundaries — it never treats connection close as an end-of-body
signal, so a server that frames responses incorrectly fails loudly instead of
appearing to work.

`make sanitize` and `make harden` build into separate directories so
instrumented objects can never end up in the normal build, and each wipes its
directory first because `make` does not track `CXXFLAGS` changes. Use
`make sanitize SANITIZERS=undefined` to run UBSan alone where the ASan runtime
cannot initialise.

Current totals: **2 238 assertions, 0 failures** — 1 928 across the six C++
unit test binaries, 220 in the integration test, 90 in the edge-case test.

| Test binary | Covers |
| --- | --- |
| `build/framing_test` | receive-buffer invariants: fragmentation, pipelining, partial bodies |
| `build/parser_test` | request line, headers, `Host`, `Content-Length`, malformed input |
| `build/response_test` | response serialisation, exact `Content-Length`, `Connection: close` |
| `build/net_test` | `send_all()` short writes and error paths |
| `build/calculator_test` | routing, integer validation, division by zero, overflow |
| `build/edge_test` | parser boundaries: truncated request line/headers, incomplete bodies left buffered, the 64 KiB head cap, the 1 MiB body cap, `Expect: 100-continue`, reassembly at every split point |
| `tests/integration_test.py` | one real connection: required matrix, keep-alive, 405, missing `Host`, pipelining (6 and 50 requests, and a batch containing a request body) |
| `tests/edge_integration_test.py` | malformed input over real sockets: 15 keep-alive error responses on one connection, a trickled request, a late body, `Expect: 100-continue`, an incomplete request abandoned by the client, an unterminated oversized head, an over-cap `Content-Length`, five untrustworthy `Content-Length` values, and `Transfer-Encoding` |

The two Python tests split by connection lifetime. `integration_test.py` holds a
single connection open for the whole run, which is what makes the pipelining
and keep-alive assertions meaningful. `edge_integration_test.py` needs a fresh
server per scenario, because the scenarios that lose framing correctly end the
connection and this server serves one client.

## Routes

All routes are `GET`. Both `a` and `b` are required.

| Route | Example | Response |
| --- | --- | --- |
| `/add` | `GET /add?a=2&b=3` | `200` body `5` |
| `/sub` | `GET /sub?a=10&b=4` | `200` body `6` |
| `/mul` | `GET /mul?a=6&b=7` | `200` body `42` |
| `/div` | `GET /div?a=9&b=3` | `200` body `3` |

A successful body is the decimal result and nothing else — no trailing newline.

Division is C++ integer division, which truncates toward zero:
`GET /div?a=7&b=2` returns `3`, not `3.5`.

### Parameter rules

- Values are strict signed decimal integers: an optional leading `-` followed
  by digits. A leading `+`, surrounding whitespace, hex, and empty values are
  all rejected with `400`.
- Values must fit in a signed 64-bit integer.
- If a parameter appears more than once, the **first** occurrence wins, so the
  result is deterministic.
- Unrecognised parameters are ignored, so `?a=2&b=3&debug=1` works.

## Status codes

| Code | When |
| --- | --- |
| `200 OK` | successful calculation |
| `400 Bad Request` | malformed request, missing `Host`, invalid `a`/`b`, divide by zero, arithmetic overflow, unusable `Content-Length`, `Transfer-Encoding`, or an oversized head/body |
| `404 Not Found` | path is not one of the four routes |
| `405 Method Not Allowed` | method is not `GET` |

Checks run in a fixed order so a given request always produces the same code:

1. **method** — `POST /add` is `405`, not `400` for a missing query string
2. **path** — `/pow?a=2&b=8` is `404`
3. **`a`**, then **`b`** — `/add?a=x&b=3` is `400`
4. **arithmetic** — `/div?a=1&b=0` is `400`

Arithmetic overflow is reported as `400` rather than wrapping, and
`LLONG_MIN / -1` is rejected because it is the one division that traps.

### Worked examples

```
GET /add?a=2&b=3      -> 200 "5"
GET /sub?a=10&b=4     -> 200 "6"
GET /mul?a=6&b=7      -> 200 "42"
GET /div?a=9&b=3      -> 200 "3"

GET /div?a=1&b=0      -> 400
GET /add?a=x&b=3      -> 400
GET /pow?a=2&b=8      -> 404
POST /add             -> 405
GET /add without Host -> 400
```

## Example exchange

Two requests sent as one write, answered in order on the same connection.

**Request**

```
GET /add?a=2&b=3 HTTP/1.1\r\n
Host: localhost\r\n
\r\n
```

**Response**

```
HTTP/1.1 200 OK\r\n
Content-Length: 1\r\n
Content-Type: text/plain\r\n
Connection: keep-alive\r\n
Date: Sat, 26 Sep 2026 18:19:54 GMT\r\n
\r\n
5
```

Every response carries an exact `Content-Length`, so a client can find the end
of the body without waiting for the connection to close.

## HTTP/1.1 persistent connection behaviour

HTTP/1.1 connections are persistent by default. The server keeps the socket
open and reads request after request until either:

- the client closes the connection (`recv()` returns 0), or
- the response carries `Connection: close` because the request stream can no
  longer be resynchronised (see below).

The response always states the connection disposition explicitly —
`Connection: keep-alive` or `Connection: close` — so the client never has to
infer it.

`Connection: close` is sent when the server cannot determine where the next
request begins, which happens for an invalid `Content-Length`, a
`Transfer-Encoding` header, or a head that exceeds the 64 KiB cap. The 400
response is fully written **first**, and only then is the socket closed.

A malformed request whose boundary *is* known — a missing `Host`, for example —
does not close the connection. The server knows exactly where the next request
starts, so it answers `400` and keeps serving. This is why
`GET /add` without `Host` returns `400` and leaves the connection usable.

## Request framing

**TCP is a byte stream. `recv()` boundaries have no HTTP meaning.**

A single `recv()` may return half a request line, one and a half requests, or
several requests plus the start of another. Nothing about where the kernel
chooses to split the stream corresponds to an HTTP message boundary. A server
that treats one `recv()` as one request works in testing and breaks in
production.

So the server keeps a persistent receive buffer and extracts requests from it
explicitly. The invariant, implemented by `http::take_one_request()` in
`src/http.cpp`:

> The server may receive multiple requests in one `recv()`, or only part of one.
> It therefore buffers received bytes and extracts **exactly one complete
> request at a time**. Bytes after that request remain buffered, untouched and
> in order, for the next iteration.

Each call consumes **at most** one request, advancing the buffer past exactly:

- the **head** — request line, headers, and the terminating `CRLFCRLF`, and
- the **body** — exactly `Content-Length` bytes, when a valid one was declared.

If the head is present but the announced body has not fully arrived, the buffer
is left **completely untouched** and the server waits for more bytes. A head
that was found is not treated as a complete request until its body is there too.

The connection loop in `serve_connection()` (`src/main.cpp`) drains every
already-complete request from the buffer *before* calling `recv()` again. That
ordering is what makes both pipelining and "a request followed by part of the
next one" work; draining after `recv()` would strand leftover bytes.

The `N left` figure in each log line is the number of bytes still buffered after
that request was extracted. In a pipelined run it counts down to zero:

Actual output for seven pipelined `GET /add` requests, all written in one
`send()`:

```
request #1: GET /add?a=2&b=2 (head=46 body=0, 276 left)
request #2: GET /add?a=3&b=3 (head=46 body=0, 230 left)
request #3: GET /add?a=4&b=4 (head=46 body=0, 184 left)
request #4: GET /add?a=5&b=5 (head=46 body=0, 138 left)
request #5: GET /add?a=6&b=6 (head=46 body=0, 92 left)
request #6: GET /add?a=7&b=7 (head=46 body=0, 46 left)
request #7: GET /add?a=8&b=8 (head=46 body=0, 0 left)
```

Socket EOF is never consulted to find a request boundary.

## Bodies and `Content-Length`

`Content-Length` is how a body is framed. The value is the **exact byte count**
of the body that follows the header block.

- No `Content-Length` means no body, so the head is the whole request.
- A present `Content-Length` means exactly that many bytes belong to this
  request, and the server consumes precisely that many before looking for the
  next request.
- A body may arrive across any number of `recv()` calls, or entirely inside one.
  The server does not care; it counts buffered bytes against the declared
  length.
- A `Content-Length` that is non-numeric, signed, comma-separated, or larger
  than the 1 MiB body cap is rejected as unusable framing: `400`, then
  `Connection: close`.
- `Transfer-Encoding` (including chunked) is **not** supported. Its presence
  means the body length is unknowable, so the server answers `400` and closes.
- Duplicate `Host` or duplicate `Content-Length` headers are rejected; a
  repeated length could otherwise mean two different things.

Response bodies are framed the same way, with `Content-Length` always set to
the exact byte length — never a character count, so a body containing `CRLF` is
still framed correctly.

Limits: request head ≤ **64 KiB**, body ≤ **1 MiB**. Exceeding the head cap
means the head can never become valid, so no amount of extra data could rescue
it; the server answers `400` and closes instead of growing its buffer forever.

## Pipelining

**Pipelining is supported.** A client may write several requests back to back
without waiting, and the server answers them in order on the same connection.

```
send:  GET /add?a=2&b=3 ...  GET /sub?a=10&b=4 ...  GET /mul?a=6&b=7 ...
recv:  200 "5"               200 "6"                200 "42"
```

This is not a special mode — it falls out of the framing invariant. Since
`take_one_request()` consumes exactly one request and leaves the rest buffered,
a batch of requests arriving together is simply drained one at a time by the
existing loop. The integration test verifies 6-request and 50-request batches,
and a batch with a request body in the middle.

The reverse also matters: a client may send part of a request and pause. The
server waits, buffer intact, until the rest arrives.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | argument handling, accept one connection, run the persistent loop |
| `src/net.cpp` | `listen`/`accept`/`send_all`, `SIGPIPE` suppression |
| `src/http.cpp` | the receive buffer, `take_one_request()`, request line and header parsing |
| `src/response.cpp` | response serialisation, `Date`, write-all |
| `src/calculator.cpp` | query parsing, integer validation, arithmetic, routing |

`net::ignore_sigpipe()` is required by every process that writes to a socket: a
client that vanishes mid-response would otherwise raise `SIGPIPE` and kill the
server, instead of returning `EPIPE` so the connection can be dropped cleanly.

## Known limitations

Deliberate scope limits, not oversights:

- **One connection per process.** The server serves a single client and exits.
  There is no concurrency and no accept loop.
- **`405` omits the `Allow` header**, which RFC 9110 requires. Status codes and
  routing are the focus here.
- **A client's `Connection: close` request header is not honoured.** The
  connection stays open and the response says `Connection: keep-alive`; the
  connection ends when the client closes it.
- **No `Transfer-Encoding` / chunked bodies.**
- **`Expect: 100-continue` is not implemented.**
- **No keep-alive or read timeouts.** A client that opens a connection and goes
  silent holds it indefinitely.
- **No percent-decoding** of query values.
- **Loopback only** — the listener is bound to `127.0.0.1`.
