#include "net.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {

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

}  // namespace

int main(int argc, char** argv) {
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

    ::close(conn_fd);
    ::close(listen_fd);
    std::printf("clean shutdown\n");
    return 0;
}
