// Unit tests for HTTP response serialisation.
//
// The invariant under test: the client can always find the end of a response
// using Content-Length alone, without the connection closing.

#include "response.h"

#include <cstdio>
#include <string>

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

// Returns the value of a header from a serialised response, or "" if absent.
std::string header_of(const std::string& raw, const std::string& name) {
    const std::string key = name + ": ";
    std::size_t pos = raw.find("\r\n");
    while (pos != std::string::npos) {
        const std::size_t begin = pos + 2;
        const std::size_t eol = raw.find("\r\n", begin);
        if (eol == std::string::npos) {
            break;
        }
        const std::string line = raw.substr(begin, eol - begin);
        if (line.compare(0, key.size(), key) == 0) {
            return line.substr(key.size());
        }
        pos = eol;
    }
    return std::string();
}

void test_status_line() {
    std::printf("1. status line\n");
    const std::string raw = http::serialize({http::kStatusOk, "5", false});
    check(raw.compare(0, 17, "HTTP/1.1 200 OK\r\n") == 0, "200 status line");

    check(http::serialize({http::kStatusBadRequest, "x", false})
                  .compare(0, 26, "HTTP/1.1 400 Bad Request\r\n") == 0,
          "400 status line");
    check(http::serialize({http::kStatusNotFound, "x", false})
                  .compare(0, 24, "HTTP/1.1 404 Not Found\r\n") == 0,
          "404 status line");
    check(http::serialize({http::kStatusMethodNotAllowed, "x", false})
                  .compare(0, 33, "HTTP/1.1 405 Method Not Allowed\r\n") == 0,
          "405 status line");

    check(std::string(http::reason_phrase(200)) == "OK", "reason 200");
    check(std::string(http::reason_phrase(400)) == "Bad Request", "reason 400");
    check(std::string(http::reason_phrase(404)) == "Not Found", "reason 404");
    check(std::string(http::reason_phrase(405)) == "Method Not Allowed", "reason 405");
    check(std::string(http::reason_phrase(999)) == "Unknown", "reason fallback");
}

void test_required_headers() {
    std::printf("2. required headers present with CRLF line endings\n");
    const std::string raw = http::serialize({http::kStatusOk, "5", false});
    check(header_of(raw, "Content-Length") == "1", "Content-Length is exact");
    check(header_of(raw, "Content-Type") == "text/plain", "Content-Type");
    check(header_of(raw, "Connection") == "keep-alive", "Connection keep-alive");
    check(raw.find("\r\nContent-Length: 1\r\n") != std::string::npos,
          "headers separated by CRLF, not bare LF");
    check(raw.find("\r\n\r\n") != std::string::npos, "blank line before body");
    // No bare LF anywhere.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            check(i > 0 && raw[i - 1] == '\r', "every LF is preceded by CR");
            break;
        }
    }
}

void test_connection_close() {
    std::printf("3. Connection: close\n");
    const std::string raw = http::serialize({http::kStatusOk, "5", true});
    check(header_of(raw, "Connection") == "close", "Connection close");
    check(raw.find("Connection: keep-alive") == std::string::npos,
          "keep-alive absent when closing");
    check(header_of(raw, "Content-Length") == "1", "Content-Length still exact");
    check(raw.size() >= 2 && raw.compare(raw.size() - 3, 3, "\r\n5") == 0,
          "body still last");
}

void test_content_length_is_exact() {
    std::printf("4. Content-Length equals the exact body byte count\n");

    struct Case {
        const char* body;
        std::size_t length;
    };
    const Case cases[] = {
        {"", 0},
        {"5", 1},
        {"42", 2},
        {"-7", 2},
        {"a\r\nb", 4},                    // CRLF inside the body
        {"a\n\nb", 4},                    // bare LF inside the body
        {"12345678901234567890", 20},     // larger than a 64-bit host's int
        {"\r\n\r\n", 4},
    };

    for (const Case& c : cases) {
        http::Response response;
        response.body = c.body;
        const std::string raw = http::serialize(response);
        const std::string declared = header_of(raw, "Content-Length");
        char expected[32];
        std::snprintf(expected, sizeof(expected), "%zu", c.length);
        check(declared == expected, "Content-Length matches body byte length");
        check(raw.size() == raw.find("\r\n\r\n") + 4 + c.length,
              "nothing appended after the body");
    }
}

void test_body_is_not_altered() {
    std::printf("5. the body is written verbatim\n");
    // A successful arithmetic result must have no trailing newline added.
    const std::string raw = http::serialize({http::kStatusOk, "5", false});
    check(raw.size() >= 1 && raw[raw.size() - 1] == '5', "no trailing newline");
    check(raw.compare(raw.size() - 5, 5, "\r\n\r\n" "5") == 0,
          "body is exactly the CRLFCRLF then the result");

    const std::string empty = http::serialize({http::kStatusBadRequest, "", false});
    check(empty.size() >= 2 && empty.compare(empty.size() - 2, 2, "\r\n") == 0,
          "empty body adds nothing after the blank line");
    check(header_of(empty, "Content-Length") == "0", "empty body -> Content-Length: 0");
}

void test_back_to_back_responses_are_unambiguous() {
    std::printf("6. concatenated responses can be split by Content-Length\n");
    // Two responses written back to back on one connection. A client that
    // honours Content-Length must be able to find the exact boundary.
    const std::string a = http::serialize({http::kStatusOk, "5", false});
    const std::string b = http::serialize({http::kStatusNotFound, "Not Found: /pow", false});
    const std::string stream = a + b;

    std::size_t pos = 0;
    int seen = 0;
    while (pos < stream.size()) {
        const std::size_t head_end = stream.find("\r\n\r\n", pos);
        if (head_end == std::string::npos) {
            check(false, "each response must have a header terminator");
            break;
        }
        const std::string head = stream.substr(pos, head_end - pos);
        const std::string len = header_of(head + "\r\n", "Content-Length");
        const std::size_t body_len = static_cast<std::size_t>(std::stoul(len));
        pos = head_end + 4 + body_len;
        ++seen;
    }
    check(seen == 2, "exactly two responses delimited by Content-Length");
    check(pos == stream.size(), "consumed the stream exactly, no bytes left");
}

}  // namespace

int main() {
    test_status_line();
    test_required_headers();
    test_connection_close();
    test_content_length_is_exact();
    test_body_is_not_altered();
    test_back_to_back_responses_are_unambiguous();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
