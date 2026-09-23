#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net {

int listen_ipv4(std::uint16_t port, std::uint16_t* bound_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        std::perror("setsockopt");
        ::close(fd);
        return -1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 16) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    if (bound_port != nullptr) {
        sockaddr_in actual;
        socklen_t len = sizeof(actual);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
            *bound_port = ntohs(actual.sin_port);
        } else {
            *bound_port = port;
        }
    }

    return fd;
}

int accept_one(int listen_fd) {
    for (;;) {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        std::perror("accept");
        return -1;
    }
}

void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            std::perror("send");
        }
        return false;
    }
    return true;
}

}  // namespace net
