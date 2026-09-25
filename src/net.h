#pragma once

#include <cstddef>
#include <cstdint>

namespace net {

// Defined below. accept_one() only needs a pointer to it.
class Shutdown;

// Creates a listening IPv4 socket bound to 127.0.0.1:port.
// On success returns fd >= 0 and writes the actually bound port to
// bound_port (useful when port 0 was requested). Returns -1 on failure.
int listen_ipv4(std::uint16_t port, std::uint16_t* bound_port);

// Accepts one connection, retrying on EINTR. Returns fd >= 0 or -1 on failure.
//
// When `shutdown` is supplied, a signal that arrives during accept() aborts the
// wait instead of being retried, so a shutdown request cannot be lost in the
// window between poll() reporting the socket readable and accept() running.
// Callers should check shutdown->requested() to tell the two -1 causes apart.
int accept_one(int listen_fd, const Shutdown* shutdown = nullptr);

// Writes exactly len bytes to fd. send() is allowed to write fewer bytes than
// requested, so this loops until everything is written, retrying on EINTR.
// Returns true on success, false on error (including a peer that already
// closed the connection).
bool send_all(int fd, const char* data, std::size_t len);

// Writing to a socket whose peer has gone away raises SIGPIPE, whose default
// action is to kill the process. Call this once at startup so that instead the
// write returns EPIPE and the connection can be dropped cleanly. Required by
// any process that calls send_all().
void ignore_sigpipe();

// Graceful shutdown on SIGINT / SIGTERM.
//
// A signal handler may only do async-signal-safe things, so this does the
// minimum there: set a flag and write() one byte to a self-pipe. The server
// parks in wait(), and that byte is what wakes it. Nothing else -- no closing,
// no printing, no allocation -- happens in the handler.
class Shutdown {
public:
    enum class Wait { kTimeout, kReadable, kRequested };

    // Installs handlers for SIGINT and SIGTERM. Returns false on failure.
    // Handlers are installed without SA_RESTART, so a blocked syscall reports
    // EINTR as well, and neither signal is left at its default (fatal) action.
    bool install();

    // Waits until `fd` is readable, or a shutdown is requested. A negative
    // timeout blocks indefinitely. Prefer this over a bare blocking recv(),
    // which cannot be interrupted in a controlled way.
    Wait wait(int fd, int timeout_ms) const;

    // True once SIGINT or SIGTERM has arrived.
    bool requested() const;

    // Closes the self-pipe. Both sockets in the server are closed by the
    // caller, which knows what they are.
    void close();

    Shutdown() = default;
    ~Shutdown();
    Shutdown(const Shutdown&) = delete;
    Shutdown& operator=(const Shutdown&) = delete;

private:
    int wake_read_ = -1;
    int wake_write_ = -1;
};

}  // namespace net
