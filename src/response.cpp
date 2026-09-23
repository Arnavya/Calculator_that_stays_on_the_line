#include "response.h"

#include <ctime>

#include "net.h"

namespace http {
namespace {

// RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT". A server with a
// clock is expected to send it. Returns an empty string if the clock fails.
std::string http_date() {
    const std::time_t now = std::time(nullptr);
    std::tm gmt;
    if (gmtime_r(&now, &gmt) == nullptr) {
        return std::string();
    }
    char buffer[40];
    if (std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &gmt) == 0) {
        return std::string();
    }
    return std::string(buffer);
}

}  // namespace

const char* reason_phrase(int status_code) {
    switch (status_code) {
        case kStatusOk:
            return "OK";
        case kStatusBadRequest:
            return "Bad Request";
        case kStatusNotFound:
            return "Not Found";
        case kStatusMethodNotAllowed:
            return "Method Not Allowed";
        default:
            return "Unknown";
    }
}

std::string serialize(const Response& response) {
    std::string out;
    out.reserve(200 + response.body.size());

    out += "HTTP/1.1 ";
    out += std::to_string(response.status_code);
    out += ' ';
    out += reason_phrase(response.status_code);
    out += "\r\n";

    // The exact byte count, not a character count, so a body containing \r\n
    // or any other byte is still framed correctly.
    out += "Content-Length: ";
    out += std::to_string(response.body.size());
    out += "\r\n";

    out += "Content-Type: text/plain\r\n";
    out += response.close_after ? "Connection: close\r\n"
                                 : "Connection: keep-alive\r\n";

    const std::string date = http_date();
    if (!date.empty()) {
        out += "Date: ";
        out += date;
        out += "\r\n";
    }

    out += "\r\n";
    out += response.body;
    return out;
}

bool send_response(int fd, const Response& response) {
    const std::string bytes = serialize(response);
    return net::send_all(fd, bytes.data(), bytes.size());
}

}  // namespace http
