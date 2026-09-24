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

// Persistent connection loop over one socket.
//
// The receive buffer is the only place bytes live between recv() and the
// parser. Each iteration first drains every request that is already complete
// in the buffer, then blocks in recv() for more. That order is what makes
// pipelining and "request + partial next request" work.
int serve_connection(int conn_fd) {
    std::string buffer;  // per-connection, persists across recv() calls
    char chunk[4096];
    int served = 0;
    int rejected = 0;
    bool stop = false;

    while (!stop) {
        // 1) Drain every request already fully buffered. This runs before
        //    recv() so leftover bytes from a previous recv() are never ignored.
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
                http::send_response(conn_fd, response);
                stop = true;
                break;
            }

            ++served;

            // A broken head still has a known boundary, so the next request
            // starts right after it and the connection can stay open. When the
            // extent is unknowable the stream cannot be resynchronised, so the
            // response goes out and only then does the connection close.
            http::Response response;
            if (parsed.error != http::ParseError::kNone) {
                ++rejected;
                std::printf("request #%d rejected: %s\n", served,
                            http::describe(parsed.error));
                response = bad_request(http::describe(parsed.error));
                response.close_after = parsed.framing_lost;
            } else {
                std::printf("request #%d: %s %s (head=%zu body=%zu, %zu left)\n",
                            served, parsed.request.method.c_str(),
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
            if (response.close_after) {
                std::printf("closing after response (Connection: close)\n");
                stop = true;
            }

        }

        if (stop) {
            break;
        }

        // 2) Ask the kernel for more bytes. Whatever arrives is appended, never
        //    assumed to align with a request boundary.
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

    std::printf("served %d requests (%d rejected) on one connection\n", served,
                rejected);
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

    std::uint16_t bound_port = 0;
    int listen_fd = net::listen_ipv4(port, &bound_port);
    if (listen_fd < 0) {
        return 1;
    }
    std::printf("listening on 127.0.0.1:%u\n", static_cast<unsigned>(bound_port));
    std::fflush(stdout);

    int conn_fd = net::accept_one(listen_fd);
    if (conn_fd < 0) {
        ::close(listen_fd);
        return 1;
    }
    std::printf("connection accepted\n");
    std::fflush(stdout);

    int rc = serve_connection(conn_fd);

    ::close(conn_fd);
    ::close(listen_fd);
    std::printf("clean shutdown\n");
    return rc;
}
