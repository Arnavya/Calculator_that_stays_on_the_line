#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net {
namespace {

// Written by the signal handler, read by wait(). sig_atomic_t is the only type
// a handler may portably touch.
volatile sig_atomic_t g_requested = 0;
int g_wake_write = -1;

// Async-signal-safe: assigns a sig_atomic_t and calls write(). Nothing else.
extern "C" void on_shutdown_signal(int) {
    g_requested = 1;
    if (g_wake_write >= 0) {
        const char byte = 'x';
        const ssize_t written = ::write(g_wake_write, &byte, 1);
        // Deliberately ignored. The write end is non-blocking, so this cannot
        // block, and a full pipe already means the wakeup is pending.
        (void)written;
    }
}

}  // namespace

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

int accept_one(int listen_fd, const Shutdown* shutdown) {
    for (;;) {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            return fd;
        }
        if (errno == EINTR) {
            // Without a Shutdown to consult, retrying is the only option. With
            // one, a signal that arrived mid-accept must not restart the wait.
            if (shutdown != nullptr && shutdown->requested()) {
                return -1;
            }
            continue;
        }
        std::perror("accept");
        return -1;
    }
}

void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

bool Shutdown::install() {
    int fds[2];
    if (::pipe(fds) != 0) {
        std::perror("pipe");
        return false;
    }
    wake_read_ = fds[0];
    wake_write_ = fds[1];
    g_wake_write = wake_write_;

    // The write end must not block the handler, and neither end should leak
    // into a child process.
    for (const int fd : {wake_read_, wake_write_}) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            int want = flags;
            if (fd == wake_write_) {
                want |= O_NONBLOCK;
            }
            ::fcntl(fd, F_SETFL, want);
        }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_shutdown_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: blocked calls should report EINTR
    if (::sigaction(SIGINT, &action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &action, nullptr) != 0) {
        std::perror("sigaction");
        return false;
    }
    return true;
}

Shutdown::Wait Shutdown::wait(int fd, int timeout_ms) const {
    struct pollfd fds[2];
    nfds_t count = 0;
    if (wake_read_ >= 0) {
        fds[count].fd = wake_read_;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        ++count;
    }
    fds[count].fd = fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    ++count;

    const int rc = ::poll(fds, count, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return g_requested ? Wait::kRequested : Wait::kTimeout;
        }
        std::perror("poll");
        return Wait::kTimeout;
    }
    if (g_requested != 0) {
        // Drain the pipe so a later wait() is not woken by a stale byte.
        if (wake_read_ >= 0) {
            char scratch[64];
            while (::read(wake_read_, scratch, sizeof(scratch)) > 0) {
            }
        }
        return Wait::kRequested;
    }
    if (rc == 0) {
        return Wait::kTimeout;
    }
    return Wait::kReadable;
}

bool Shutdown::requested() const { return g_requested != 0; }

void Shutdown::close() {
    if (wake_read_ >= 0) {
        ::close(wake_read_);
        wake_read_ = -1;
    }
    if (wake_write_ >= 0) {
        ::close(wake_write_);
        wake_write_ = -1;
    }
    g_wake_write = -1;
}

Shutdown::~Shutdown() { close(); }

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
