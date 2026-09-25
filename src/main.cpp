#include "calculator.h"
#include "http.h"
#include "net.h"
#include "response.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

http::Response bad_request(const char* detail) {
    http::Response response;
    response.status_code = http::kStatusBadRequest;
    response.body = std::string("Bad Request: ") + detail;
    return response;
}

bool parse_port(const char* text, std::uint16_t* out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 65535) {
        return false;
    }
    *out = static_cast<std::uint16_t>(value);
    return true;
}

// Counts kept only so the shutdown summary can report what the server did.
// Deliberately four integers rather than a metrics subsystem.
struct Stats {
    int connections_accepted = 0;
    int requests_processed = 0;
    int requests_rejected = 0;
    int responses_sent = 0;
};

// Persistent connection loop over one socket.
//
// The receive buffer is the only place bytes live between recv() and the
// parser. Each iteration first drains every request that is already complete
// in the buffer, then waits for more. That order is what makes pipelining and
// "request + partial next request" work.
//
// recv() is never called while it would block indefinitely: the wait goes
// through Shutdown::wait(), so a signal arriving mid-connection ends the loop
// instead of being ignored until the next client byte happens to arrive.
int serve_connection(int conn_fd, const net::Shutdown& shutdown, Stats* stats) {
    std::string buffer;  // per-connection, persists across recv() calls
    char chunk[4096];
    bool stop = false;

    while (!stop) {
        // 1) Drain every request already fully buffered. This runs before
        //    waiting for more so leftover bytes from a previous recv() are
        //    never ignored.
        for (;;) {
            const http::ParseResult parsed = http::take_one_request(buffer);

            if (parsed.status == http::FrameStatus::kIncomplete) {
                break;  // need more bytes
            }
            if (parsed.status == http::FrameStatus::kHeadTooLarge) {
                std::printf("rejecting request: %s\n",
                            http::describe(parsed.error));
                std::fflush(stdout);
                http::Response response = bad_request(http::describe(parsed.error));
                response.close_after = true;
                // The full response goes out first; only then do we stop.
                if (http::send_response(conn_fd, response)) {
                    ++stats->responses_sent;
                }
                stop = true;
                break;
            }

            ++stats->requests_processed;

            // A broken head still has a known boundary, so the next request
            // starts right after it and the connection can stay open. When the
            // extent is unknowable the stream cannot be resynchronised, so the
            // response goes out and only then does the connection close.
            http::Response response;
            if (parsed.error != http::ParseError::kNone) {
                ++stats->requests_rejected;
                std::printf("request #%d rejected: %s\n",
                            stats->requests_processed,
                            http::describe(parsed.error));
                response = bad_request(http::describe(parsed.error));
                response.close_after = parsed.framing_lost;
            } else {
                std::printf("request #%d: %s %s (head=%zu body=%zu, %zu left)\n",
                            stats->requests_processed, parsed.request.method.c_str(),
                            parsed.request.target.c_str(),
                            parsed.request.head_length, parsed.request.body_length,
                            buffer.size());
                response = calc::handle(parsed.request);
            }
            std::fflush(stdout);

            if (!http::send_response(conn_fd, response)) {
                stop = true;  // peer vanished mid-response
                break;
            }
            ++stats->responses_sent;
            if (response.close_after) {
                std::printf("closing after response (Connection: close)\n");
                stop = true;
            }
        }

        if (stop) {
            break;
        }

        // 2) Wait for more bytes, but stay interruptible by a signal. Whatever
        //    arrives is appended, never assumed to align with a request
        //    boundary.
        const net::Shutdown::Wait wait = shutdown.wait(conn_fd, -1);
        if (wait == net::Shutdown::Wait::kRequested) {
            std::printf("shutdown requested while connected\n");
            std::fflush(stdout);
            break;
        }
        if (wait != net::Shutdown::Wait::kReadable) {
            break;  // kTimeout cannot happen with a negative timeout
        }

        const ssize_t n = ::recv(conn_fd, chunk, sizeof(chunk), 0);

        if (n == 0) {
            std::printf("client closed connection\n");
            std::fflush(stdout);
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("recv");
            break;
        }

        buffer.append(chunk, static_cast<std::size_t>(n));
    }

    // Reported for every connection, including the ordinary case where the
    // client simply hangs up. The shutdown summary in main() is additive and
    // only appears when a signal ended the run.
    std::printf("served %d requests (%d rejected) on one connection\n",
                stats->requests_processed, stats->requests_rejected);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // A client may disconnect at any point; writing to that socket must return
    // an error, not kill the process.
    net::ignore_sigpipe();

    const char* port_arg = (argc > 1) ? argv[1] : "8080";

    std::uint16_t port = 8080;
    if (!parse_port(port_arg, &port)) {
        std::fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }

    net::Shutdown shutdown;
    if (!shutdown.install()) {
        return 1;
    }

    std::uint16_t bound_port = 0;
    int listen_fd = net::listen_ipv4(port, &bound_port);
    if (listen_fd < 0) {
        return 1;
    }
    std::printf("listening on 127.0.0.1:%u\n", static_cast<unsigned>(bound_port));
    std::fflush(stdout);

    Stats stats;

    // Wait for a connection interruptibly, so Ctrl-C before anyone connects is
    // a clean exit rather than a process that only dies when interrupted again.
    int conn_fd = -1;
    bool interrupted = false;
    for (;;) {
        const net::Shutdown::Wait wait = shutdown.wait(listen_fd, -1);
        if (wait == net::Shutdown::Wait::kRequested) {
            interrupted = true;
            break;
        }
        if (wait != net::Shutdown::Wait::kReadable) {
            break;
        }
        conn_fd = net::accept_one(listen_fd, &shutdown);
        break;
    }

    if (conn_fd < 0) {
        // Either a shutdown, or a genuine accept() failure.
        if (!interrupted && !shutdown.requested()) {
            ::close(listen_fd);
            return 1;
        }
        interrupted = true;
    } else {
        ++stats.connections_accepted;
        std::printf("connection accepted\n");
        std::fflush(stdout);

        const int rc = serve_connection(conn_fd, shutdown, &stats);
        ::close(conn_fd);
        if (rc != 0) {
            ::close(listen_fd);
            return rc;
        }
    }

    // Stop accepting first, then release the listening socket. Order matters:
    // closing the listener before the client socket is what stops a new
    // connection from being queued while this one is being torn down.
    ::close(listen_fd);

    // A signal can arrive at three different points -- waiting to accept,
    // serving a connection, or just after either finished -- so the summary is
    // keyed off the flag rather than off which branch ran.
    if (shutdown.requested()) {
        interrupted = true;
    }
    if (interrupted) {
        std::printf("connections accepted: %d\n", stats.connections_accepted);
        // "malformed" counts requests the HTTP layer refused, not routing
        // outcomes: a 404 or 405 is a well-formed request answered with an
        // error, so it is counted as processed and not as malformed.
        std::printf("requests processed:   %d (%d malformed)\n",
                    stats.requests_processed, stats.requests_rejected);
        std::printf("responses sent:       %d\n", stats.responses_sent);
    }
    std::printf("clean shutdown\n");
    shutdown.close();
    return 0;
}
