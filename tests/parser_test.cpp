// Unit tests for HTTP/1.1 request parsing and validation.

#include "http.h"

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

http::ParseResult feed(const std::string& raw) {
    std::string buffer = raw;
    return http::take_one_request(buffer);
}

void expect_valid(const std::string& raw, const char* method,
                  const char* target, const char* what) {
    const http::ParseResult r = feed(raw);
    check(r.status == http::FrameStatus::kComplete, what);
    if (r.status != http::FrameStatus::kComplete) {
        return;
    }
    check(r.error == http::ParseError::kNone, what);
    check(r.framing_lost == false, what);
    check(r.request.method == method, what);
    check(r.request.target == target, what);
    check(r.request.version == "HTTP/1.1", what);
    check(r.request.body.empty(), what);
}

void expect_error(const std::string& raw, http::ParseError expected,
                  bool expect_framing_lost, const char* what) {
    const http::ParseResult r = feed(raw);
    check(r.status == http::FrameStatus::kComplete, what);
    if (r.status != http::FrameStatus::kComplete) {
        return;
    }
    check(r.error == expected, what);
    if (expected != http::ParseError::kNone && r.error != expected) {
        std::printf("        (got: %s)\n", http::describe(r.error));
    }
    check(r.framing_lost == expect_framing_lost, what);
}

void test_valid_requests() {
    std::printf("1. valid GET requests parse\n");
    expect_valid("GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET",
                 "/add?a=2&b=3", "simple GET parses");
    expect_valid("GET / HTTP/1.1\r\nHost: x\r\n\r\n", "GET", "/", "root path parses");
    expect_valid("GET /mul?a=6&b=7 HTTP/1.1\r\nHost: a\r\n\r\n", "GET",
                 "/mul?a=6&b=7", "query string preserved verbatim");
    expect_valid("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "POST",
                 "/add", "POST parses (routing decides 405, not the parser)");
    expect_valid("GET /add HTTP/1.1\r\nHost: x\r\nX-Empty:\r\n\r\n", "GET", "/add",
                 "header with empty value is legal");
    expect_valid("GET /add HTTP/1.1\r\nHost:    x   \r\n\r\n", "GET", "/add",
                 "OWS around header value is trimmed");
    expect_valid("GET /add HTTP/1.1\r\nHOST: x\r\n\r\n", "GET", "/add",
                 "header names are case-insensitive");
    expect_valid("GET /add HTTP/1.1\r\nHost: x\r\nAccept: a\r\nAccept: b\r\n\r\n",
                 "GET", "/add", "repeated non-singleton headers allowed");
    expect_valid("GET /add HTTP/1.1\r\nHost: example.com:8080\r\n\r\n", "GET", "/add",
                 "Host value is not restricted to localhost");
}

void test_request_line() {
    std::printf("2. request line validation\n");
    expect_error("GET\r\nHost: x\r\n\r\n", http::ParseError::kMalformedRequestLine,
                 false, "missing version -> 400");
    expect_error("GET /\r\nHost: x\r\n\r\n", http::ParseError::kMalformedRequestLine,
                 false, "missing version -> 400");
    expect_error("GET  / HTTP/1.1\r\nHost: x\r\n\r\n",
                 http::ParseError::kMalformedRequestLine, false,
                 "double space in request line -> 400");
    expect_error("GET /a b HTTP/1.1\r\nHost: x\r\n\r\n",
                 http::ParseError::kMalformedRequestLine, false,
                 "space inside target -> 400");
    expect_error(" / HTTP/1.1\r\nHost: x\r\n\r\n",
                 http::ParseError::kMalformedRequestLine, false,
                 "empty method -> 400");
    expect_error("GET add HTTP/1.1\r\nHost: x\r\n\r\n",
                 http::ParseError::kMalformedRequestLine, false,
                 "target must be origin-form -> 400");
    expect_error("G@T / HTTP/1.1\r\nHost: x\r\n\r\n",
                 http::ParseError::kMalformedRequestLine, false,
                 "non-token character in method -> 400");
    expect_error("GET / HTTP/1.0\r\nHost: x\r\n\r\n",
                 http::ParseError::kUnsupportedVersion, false,
                 "HTTP/1.0 rejected (this server speaks HTTP/1.1)");
    expect_error("GET / HTTP/2.0\r\nHost: x\r\n\r\n",
                 http::ParseError::kUnsupportedVersion, false, "HTTP/2 rejected");
    expect_error("GET / HTTP/1.1x\r\nHost: x\r\n\r\n",
                 http::ParseError::kUnsupportedVersion, false,
                 "trailing junk after version -> 400");
}

void test_headers() {
    std::printf("3. header validation\n");
    expect_error("GET / HTTP/1.1\r\nHost\r\n\r\n", http::ParseError::kMalformedHeader,
                 false, "header with no colon -> 400");
    expect_error("GET / HTTP/1.1\r\n: x\r\n\r\n", http::ParseError::kMalformedHeader,
                 false, "empty header name -> 400");
    expect_error("GET / HTTP/1.1\r\nHost : x\r\n\r\n",
                 http::ParseError::kMalformedHeader, false,
                 "whitespace before colon -> 400");
    expect_error("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n",
                 http::ParseError::kMalformedHeader, false,
                 "obsolete line folding -> 400");
    expect_error("GET / HTTP/1.1\r\nHo st: x\r\n\r\n",
                 http::ParseError::kMalformedHeader, false,
                 "space in header name -> 400");
    expect_error("GET / HTTP/1.1\r\nHost: x\r\nX-Bad: a\x01""b\r\n\r\n",
                 http::ParseError::kMalformedHeader, false,
                 "control character in value -> 400");
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
                 http::ParseError::kDuplicateHeader, false, "duplicate Host -> 400");
    expect_error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
                 "Content-Length: 0\r\n\r\n",
                 http::ParseError::kDuplicateHeader, false,
                 "duplicate Content-Length -> 400");
}

void test_host() {
    std::printf("4. Host requirement\n");
    expect_error("GET /add HTTP/1.1\r\n\r\n", http::ParseError::kMissingHost, false,
                 "missing Host -> 400 and connection stays usable");
    expect_error("GET /add HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
                 http::ParseError::kMissingHost, false, "missing Host -> 400");
    expect_valid("GET /add HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET", "/add",
                 "Host present -> valid");
}

void test_content_length() {
    std::printf("5. Content-Length framing\n");
    {
        const http::ParseResult r = feed("POST /x HTTP/1.1\r\nHost: h\r\n"
                                         "Content-Length: 5\r\n\r\nhello");
        check(r.status == http::FrameStatus::kComplete, "body complete -> consumed");
        check(r.error == http::ParseError::kNone, "body complete -> no error");
        check(r.request.body == "hello", "body bytes exact");
        check(r.request.body_length == 5, "body length exact");
    }
    {
        // Body short by one byte: must stay incomplete, buffer untouched.
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhell";
        const std::size_t before = buffer.size();
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete, "short body -> incomplete");
        check(buffer.size() == before, "short body -> buffer untouched");
    }
    {
        // Absent Content-Length means a zero-length body.
        const http::ParseResult r = feed("GET /x HTTP/1.1\r\nHost: h\r\n\r\n");
        check(r.request.body_length == 0, "absent Content-Length -> zero-length body");
    }
    {
        // Body followed by another request: the second must survive intact.
        const std::string second = "GET /y HTTP/1.1\r\nHost: h\r\n\r\n";
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello" + second;
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "request with body consumed");
        check(r.request.body == "hello", "body is exactly 5 bytes, not more");
        check(buffer == second, "next request preserved byte-for-byte");
    }
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: abc\r\n\r\n",
                 http::ParseError::kInvalidContentLength, true,
                 "non-numeric Content-Length -> 400, framing lost");
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n",
                 http::ParseError::kInvalidContentLength, true,
                 "negative Content-Length -> 400, framing lost");
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5, 5\r\n\r\n",
                 http::ParseError::kInvalidContentLength, true,
                 "comma-separated Content-Length -> 400, framing lost");
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length:\r\n\r\n",
                 http::ParseError::kInvalidContentLength, true,
                 "empty Content-Length -> 400, framing lost");
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999999\r\n\r\n",
                 http::ParseError::kInvalidContentLength, true,
                 "overflowing Content-Length -> 400, framing lost");
    expect_error("POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n",
                 http::ParseError::kUnsupportedTransferEncoding, true,
                 "chunked is not implemented -> 400, framing lost");
}

void test_fragmented_body() {
    std::printf("6. body arriving one byte at a time\n");
    const std::string head = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n";
    const std::string body = "abcd";
    const std::string tail = "GET /z HTTP/1.1\r\nHost: h\r\n\r\n";

    std::string buffer;
    for (std::size_t i = 0; i < head.size(); ++i) {
        buffer.push_back(head[i]);
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete,
              "still incomplete while head trickles in");
    }
    for (std::size_t i = 0; i + 1 < body.size(); ++i) {
        buffer.push_back(body[i]);
        check(http::take_one_request(buffer).status == http::FrameStatus::kIncomplete,
              "still incomplete until the full body arrives");
    }
    buffer += body.substr(body.size() - 1);
    buffer += tail;

    const http::ParseResult r = http::take_one_request(buffer);
    check(r.status == http::FrameStatus::kComplete, "complete on the last body byte");
    check(r.error == http::ParseError::kNone, "no error");
    check(r.request.body == body, "fragmented body reassembled exactly");
    check(buffer == tail, "following request untouched");
}

void test_error_then_next_request() {
    std::printf("7. an errored request does not eat the next one\n");
    const std::string bad = "GET /add HTTP/1.1\r\n\r\n";  // no Host
    const std::string good = "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string buffer = bad + good;

    http::ParseResult r = http::take_one_request(buffer);
    check(r.error == http::ParseError::kMissingHost, "first request rejected");
    check(r.framing_lost == false, "missing Host keeps the stream parseable");
    check(buffer == good, "second request still intact after the error");

    r = http::take_one_request(buffer);
    check(r.status == http::FrameStatus::kComplete, "second request parses");
    check(r.error == http::ParseError::kNone, "second request is valid");
    check(r.request.target == "/add?a=2&b=3", "second request target");
    check(buffer.empty(), "buffer drained");
}

void test_malformed_head_keeps_stream_parseable() {
    std::printf("8. malformed head still has a known boundary\n");
    const std::string bad = "GET /add HTTP/1.1\r\nBadHeader\r\n\r\n";
    const std::string good = "GET /ok HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string buffer = bad + good;

    http::ParseResult r = http::take_one_request(buffer);
    check(r.error == http::ParseError::kMalformedHeader, "malformed header detected");
    check(r.framing_lost == false,
          "CRLFCRLF was found, so the next request start is known exactly");
    check(buffer == good, "following request preserved");

    r = http::take_one_request(buffer);
    check(r.error == http::ParseError::kNone, "next request parses cleanly");
    check(r.request.target == "/ok", "next request target");
}

void test_pipelined_with_body() {
    std::printf("9. pipelined requests with bodies keep their order\n");
    const std::string r1 = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
    const std::string r2 = "GET /y HTTP/1.1\r\nHost: h\r\n\r\n";
    const std::string r3 = "POST /z HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nde";
    std::string buffer = r1 + r2 + r3;

    std::string order;
    for (;;) {
        const http::ParseResult p = http::take_one_request(buffer);
        if (p.status != http::FrameStatus::kComplete) {
            break;
        }
        order += p.request.target + ":" + p.request.body + ";";
    }
    check(order == "/x:abc;/y:;/z:de;", "three pipelined requests in order with bodies");
    check(buffer.empty(), "nothing left over");
}

void test_large_body_fragments() {
    std::printf("10. a body split across many recv() calls\n");
    const std::string body(3000, 'z');
    const std::string head = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 3000\r\n\r\n";

    std::string buffer;
    http::ParseResult r;
    // Feed the head one byte at a time.
    for (std::size_t i = 0; i < head.size(); ++i) {
        buffer.push_back(head[i]);
        r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete, "head trickling in");
    }
    // Feed the body in odd-sized chunks. The chunk that completes the body is
    // expected to produce a complete request, not an incomplete one.
    const std::size_t sizes[] = {1, 999, 2, 1500, 498};
    std::size_t offset = 0, i = 0;
    while (offset < body.size()) {
        std::size_t n = sizes[i++ % 5];
        if (n > body.size() - offset) {
            n = body.size() - offset;
        }
        buffer.append(body, offset, n);
        offset += n;
        r = http::take_one_request(buffer);
        check(r.status == (offset < body.size() ? http::FrameStatus::kIncomplete
                                                : http::FrameStatus::kComplete),
              "body arriving, completing exactly on the last byte");
    }
    check(r.status == http::FrameStatus::kComplete, "completes on the final byte");
    check(r.request.body == body, "3000-byte body reassembled exactly");
    check(buffer.empty(), "buffer drained");
}

}  // namespace

int main() {
    test_valid_requests();
    test_request_line();
    test_headers();
    test_host();
    test_content_length();
    test_fragmented_body();
    test_error_then_next_request();
    test_malformed_head_keeps_stream_parseable();
    test_pipelined_with_body();
    test_large_body_fragments();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
