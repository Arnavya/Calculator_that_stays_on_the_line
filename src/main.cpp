#include "http.h"
#include "net.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// STEP 2/3 placeholder. Framing works; routing and the real body come later.
const char kPlaceholderResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "ok";

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
    bool stop = false;

    while (!stop) {
        // 1) Drain every request already fully buffered. This runs before
        //    recv() so leftover bytes from a previous recv() are never ignored.
        for (;;) {
            http::FramedRequest framed;
            const http::FrameStatus status = http::take_one_request(buffer, &framed);

            if (status == http::FrameStatus::kIncomplete) {
                break;  // need more bytes
            }
            if (status == http::FrameStatus::kHeadTooLarge) {
                std::printf("head exceeded %zu bytes, dropping connection\n",
                            http::kMaxHeadBytes);
                std::fflush(stdout);
                stop = true;
                break;
            }

            ++served;
            std::printf("request #%d: %zu bytes, %zu bytes left buffered\n",
                        served, framed.head_length, buffer.size());
            std::fflush(stdout);

            if (!net::send_all(conn_fd, kPlaceholderResponse,
                               sizeof(kPlaceholderResponse) - 1)) {
                stop = true;
                break;
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

    std::printf("served %d requests on one connection\n", served);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // A client may disconnect at any point; writing to that socket must return
    // an error, not kill the process.
    ::signal(SIGPIPE, SIG_IGN);

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
