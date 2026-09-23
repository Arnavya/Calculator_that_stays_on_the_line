#pragma once

#include <string>

namespace http {

// The status codes this server emits.
inline constexpr int kStatusOk = 200;
inline constexpr int kStatusBadRequest = 400;
inline constexpr int kStatusNotFound = 404;
inline constexpr int kStatusMethodNotAllowed = 405;

const char* reason_phrase(int status_code);

struct Response {
    int status_code = kStatusOk;
    // Success bodies are the decimal result only, with no added newline.
    std::string body;

    // When true the response carries "Connection: close" instead of
    // "Connection: keep-alive", and the caller must close the socket only
    // after every response byte has been written.
    bool close_after = false;
};

// Serialises a complete response: status line, headers, blank line, body.
//
// Content-Length is always the exact byte length of `body`, so the client can
// find the end of this response without relying on the connection closing.
std::string serialize(const Response& response);

// Serialises the response and writes all of it with send_all(), which loops
// until every byte has been handed to the kernel. Returns true only if the
// whole response was written; on false the peer went away mid-response and the
// connection must be abandoned.
bool send_response(int fd, const Response& response);

}  // namespace http
