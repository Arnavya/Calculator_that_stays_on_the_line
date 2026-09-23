#pragma once

#include <cstddef>
#include <cstdint>

namespace net {

// Creates a listening IPv4 socket bound to 127.0.0.1:port.
// On success returns fd >= 0 and writes the actually bound port to
// bound_port (useful when port 0 was requested). Returns -1 on failure.
int listen_ipv4(std::uint16_t port, std::uint16_t* bound_port);

// Accepts one connection, retrying on EINTR. Returns fd >= 0 or -1 on failure.
int accept_one(int listen_fd);

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

}  // namespace net
