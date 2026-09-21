#include "net.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// STEP 2 placeholder. Framing is NOT implemented yet: this reply is sent once
// per successful recv(), which is exactly the "one recv() == one request"
// assumption that STEP 3 removes.
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

// Persistent connection loop. Stays on one socket for the life of the
// connection: recv, reply, repeat. Only a client-side close (recv == 0) or a
// socket error ends the loop.
int serve_connection(int conn_fd) {
    char buf[4096];
    int batches = 0;

    for (;;) {
        ssize_t n = ::recv(conn_fd, buf, sizeof(buf), 0);

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

        ++batches;
        std::printf("recv batch #%d: %zd bytes\n", batches, n);
        std::fflush(stdout);

        if (!net::send_all(conn_fd, kPlaceholderResponse,
                           sizeof(kPlaceholderResponse) - 1)) {
            break;
        }
    }

    std::printf("served %d recv batches on one connection\n", batches);
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
