// Unit tests for the socket layer, focused on send_all()'s short-write loop.
//
// send() is allowed to write fewer bytes than requested. These tests use a
// socketpair with a deliberately tiny send buffer and a payload far larger
// than any socket buffer, so the loop is forced to iterate many times.

#include "net.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

// Writes `payload` from `from` with send_all() while a thread drains `to`, so a
// payload larger than the socket buffer cannot deadlock. Returns what the
// reader received.
std::string round_trip(const std::string& payload, int send_buffer_bytes) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return std::string();
    }

    int size = send_buffer_bytes;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

    // The reader must run concurrently: a payload larger than the socket
    // buffer cannot be written in one go, and nothing drains it otherwise.
    std::string received;
    std::thread reader([&] {
        char buffer[8192];
        for (;;) {
            const ssize_t n = ::recv(fds[1], buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(n));
        }
    });

    const bool write_ok = net::send_all(fds[0], payload.data(), payload.size());
    check(write_ok, "send_all reports success");
    ::shutdown(fds[0], SHUT_WR);
    reader.join();

    ::close(fds[0]);
    ::close(fds[1]);
    return received;
}

void test_small_write() {
    std::printf("1. small payload\n");
    const std::string payload = "hello";
    check(round_trip(payload, 4096) == payload, "all 5 bytes arrive intact");
}

void test_exact_buffer_size() {
    std::printf("2. payload the socket could swallow in one send()\n");
    const std::string payload(4000, 'x');
    check(round_trip(payload, 65536) == payload, "4000 bytes arrive intact");
}

void test_forces_many_partial_writes() {
    std::printf("3. payload far larger than the socket buffer\n");
    // 4 MB through a 4 KB buffer means send() must return short many hundreds
    // of times. If send_all() did not loop, this would truncate.
    const std::string payload(4u * 1024u * 1024u, 'z');
    const std::string received = round_trip(payload, 4096);
    check(received.size() == payload.size(), "every byte of 4 MB was written");
    check(received == payload, "4 MB arrives byte-identical and in order");
}

void test_zero_length_is_a_no_op() {
    std::printf("4. zero-length write\n");
    int fds[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair created");
    check(net::send_all(fds[0], "", 0), "zero-length send_all succeeds");
    ::close(fds[0]);
    ::close(fds[1]);
}

void test_error_on_closed_peer() {
    std::printf("5. writing to a closed peer fails instead of hanging\n");
    int fds[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair created");
    ::close(fds[1]);
    check(!net::send_all(fds[0], "x", 1), "send_all returns false on a dead peer");
    ::close(fds[0]);
}

void test_interleaved_sizes() {
    std::printf("6. many writes of assorted sizes keep their boundaries\n");
    // Exercises the case where one logical message is split across several
    // send_all() calls, which is what a response larger than the buffer does.
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        check(false, "socketpair created");
        return;
    }
    int size = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));

    std::vector<std::string> messages;
    for (int i = 0; i < 50; ++i) {
        messages.push_back("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n" +
                           std::string(1, static_cast<char>('a' + (i % 26))) +
                           std::string(static_cast<std::size_t>(i) * 37, '.'));
    }

    std::string expected;
    for (const std::string& m : messages) {
        expected += m;
    }

    std::string received;
    std::thread reader([&] {
        char buffer[8192];
        for (;;) {
            const ssize_t n = ::recv(fds[1], buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(n));
        }
    });

    bool all_ok = true;
    for (const std::string& m : messages) {
        all_ok = net::send_all(fds[0], m.data(), m.size()) && all_ok;
    }
    check(all_ok, "all 50 send_all calls succeeded");
    ::shutdown(fds[0], SHUT_WR);
    reader.join();

    check(received == expected, "50 messages arrive intact and in order");
    ::close(fds[0]);
    ::close(fds[1]);
}

}  // namespace

int main() {
    // Unbuffered: these tests can block, and progress must be visible.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // This process calls send_all(), so it needs the same SIGPIPE setup the
    // server does; otherwise test 5 dies with signal 13 instead of returning.
    net::ignore_sigpipe();
    test_small_write();
    test_exact_buffer_size();
    test_forces_many_partial_writes();
    test_zero_length_is_a_no_op();
    test_error_on_closed_peer();
    test_interleaved_sizes();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
