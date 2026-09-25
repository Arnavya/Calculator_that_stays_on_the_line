// Unit tests for parser boundaries and incomplete input.
//
// Complements parser_test.cpp, which covers the shape of a well-formed request
// and the error taxonomy. This file concentrates on the two things that must
// never go wrong when input is unfinished or out of bounds:
//
//   1. an incomplete request leaves the buffer byte-for-byte untouched, so the
//      bytes are still there when the rest arrives;
//   2. a request that can never become valid is reported as such, rather than
//      growing the buffer forever.

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

// Feeds `raw` and requires it to be reported as still incomplete, with the
// buffer unchanged. This is the invariant that makes fragmentation safe: an
// unfinished request must not be partially consumed.
void expect_incomplete_untouched(const std::string& raw, const char* what) {
    std::string buffer = raw;
    const std::size_t before = buffer.size();
    const http::ParseResult r = http::take_one_request(buffer);
    check(r.status == http::FrameStatus::kIncomplete, what);
    check(buffer.size() == before, what);
    check(buffer == raw, what);
}

void test_truncated_request_line() {
    std::printf("1. truncated request line is incomplete, not an error\n");
    // No CRLFCRLF anywhere yet, so the head cannot be delimited. Every prefix
    // of a valid request line must be reported as incomplete.
    const char* prefixes[] = {
        "G",
        "GE",
        "GET",
        "GET ",
        "GET /",
        "GET /add",
        "GET /add ",
        "GET /add?a=2&b=3",
        "GET /add?a=2&b=3 ",
        "GET /add?a=2&b=3 HTTP/1.1",
        "GET /add?a=2&b=3 HTTP/1.1\r",
        "GET /add?a=2&b=3 HTTP/1.1\r\n",
        "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h",
        "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r",
        "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n",
    };
    for (const char* prefix : prefixes) {
        expect_incomplete_untouched(prefix, "request-line prefix stays buffered");
    }
    // The head is only complete once the blank line lands.
    const std::string full = "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string buffer = full.substr(0, full.size() - 1);
    check(http::take_one_request(buffer).status == http::FrameStatus::kIncomplete,
          "missing the final CRLF is still incomplete");
    buffer += "\n";
    const http::ParseResult r = http::take_one_request(buffer);
    check(r.status == http::FrameStatus::kComplete, "the last CRLF completes it");
    check(r.request.target == "/add?a=2&b=3", "target intact after late completion");
    check(buffer.empty(), "buffer drained exactly");
}

void test_truncated_body() {
    std::printf("2. truncated body leaves the head buffered too\n");
    const std::string head = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n";
    // The head alone is already delimited, but the body is short, so the whole
    // request -- head included -- must stay buffered.
    expect_incomplete_untouched(head, "head without its body stays buffered");
    expect_incomplete_untouched(head + "abc", "partial body stays buffered");
    expect_incomplete_untouched(head + "abcdefghi", "body one byte short stays buffered");

    // A declared body of 0 is complete immediately.
    std::string zero = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
    const http::ParseResult r = http::take_one_request(zero);
    check(r.status == http::FrameStatus::kComplete, "Content-Length: 0 is complete");
    check(r.request.body.empty(), "Content-Length: 0 yields an empty body");

    // A head whose terminator is present but whose body is one byte short.
    const std::string partial_terminator =
        "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\na";
    expect_incomplete_untouched(partial_terminator, "body one byte short stays buffered");
    // The complement: the very next byte completes it, so this pair proves the
    // boundary is the declared length and nothing else.
    {
        std::string buffer = partial_terminator + "b";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "final body byte completes it");
        check(r.request.body == "ab", "body is exactly the 2 declared bytes");
        check(buffer.empty(), "buffer drained exactly");
    }
}

void test_oversized_body() {
    std::printf("3. Content-Length beyond the body cap is unusable framing\n");
    // A syntactically valid but over-large length cannot be resynchronised, so
    // it must be an error with framing_lost, not a request to buffer 1 MiB+.
    {
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 1048577\r\n\r\n";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "head consumed to report the error");
        check(r.error == http::ParseError::kInvalidContentLength, "over-cap length rejected");
        check(r.framing_lost, "over-cap length loses framing");
    }
    // Exactly at the cap is allowed: still waiting for the body, not an error.
    {
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 1048576\r\n\r\n";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete, "exactly at the cap is not an error");
        check(r.error == http::ParseError::kNone, "exactly at the cap has no error");
    }
    // The boundary must be enforced on the declared length, not on what has
    // arrived: a huge claim with a few bytes present is still over the cap.
    {
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 2000000\r\n\r\nxx";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.error == http::ParseError::kInvalidContentLength,
              "over-cap length rejected even with some body present");
        check(r.framing_lost, "over-cap length loses framing");
        check(buffer == "xx", "the unusable body bytes are not swallowed silently");
    }
    // Just under the cap is a legitimate (if large) request still waiting.
    {
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 1048575\r\n\r\nxx";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete, "just under the cap waits");
        check(r.error == http::ParseError::kNone, "just under the cap has no error");
    }
}

void test_head_never_completes() {
    std::printf("4. a head with no terminator cannot be rescued by more bytes\n");
    // At the cap: still merely incomplete.
    {
        std::string buffer(http::kMaxHeadBytes, 'A');
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete, "head at the cap waits for more");
    }
    // One byte over: no future byte can make this valid.
    {
        std::string buffer(http::kMaxHeadBytes + 1, 'A');
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kHeadTooLarge, "head over the cap is refused");
        check(r.error == http::ParseError::kHeadTooLarge, "reports the size error");
        check(r.framing_lost, "an unterminated over-cap head cannot be resynchronised");
    }
    // A terminator that arrives past the cap: the head IS delimited, so this is
    // an ordinary 400 rather than a close. That is the better outcome -- the
    // next request's start is known exactly, so the connection can be reused.
    // The cap exists to stop unbounded growth of an unterminated head, not to
    // reject large-but-delimited requests.
    {
        std::string buffer(http::kMaxHeadBytes + 4, 'A');
        buffer += "\r\n\r\n";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "a delimited over-cap head is still a head");
        check(r.error == http::ParseError::kMalformedRequestLine, "over-cap junk is a bad request line");
        check(r.framing_lost == false, "delimited over-cap head keeps the stream parseable");
        check(buffer.empty(), "consumed exactly, nothing left over");
    }
}

void test_expect_100_continue() {
    std::printf("5. Expect: 100-continue does not disturb framing\n");
    // The server does not implement 100-continue and does not reject the header.
    // What matters is that treating it as an ordinary header cannot shift the
    // body boundary or swallow the next request.
    const std::string next = "GET /add?a=2&b=3 HTTP/1.1\r\nHost: h\r\n\r\n";
    {
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                             "Content-Length: 5\r\n\r\nhello" +
                             next;
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "request with Expect consumed");
        check(r.error == http::ParseError::kNone, "Expect header alone is not an error");
        check(r.request.body == "hello", "body is exactly the 5 declared bytes");
        check(buffer == next, "the following request is preserved byte-for-byte");
    }
    {
        // A body announced but not yet sent: the server must wait rather than
        // answer, and must not consume the bytes of a later request.
        std::string buffer = "POST /x HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                             "Content-Length: 5\r\n\r\n";
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kIncomplete,
              "Expect does not make the server answer before the body arrives");
        check(buffer.find(next) == std::string::npos, "nothing appended on its own");
    }
    {
        // A body-bearing request with Expect, pipelined between two others.
        const std::string a = "GET /mul?a=6&b=7 HTTP/1.1\r\nHost: h\r\n\r\n";
        const std::string b = "POST /y HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                              "Content-Length: 2\r\n\r\nhi";
        const std::string c = "GET /sub?a=10&b=4 HTTP/1.1\r\nHost: h\r\n\r\n";
        std::string buffer = a + b + c;
        std::string order;
        for (;;) {
            const http::ParseResult p = http::take_one_request(buffer);
            if (p.status != http::FrameStatus::kComplete) {
                break;
            }
            order += p.request.target + ":" + p.request.body + ";";
        }
        check(order == "/mul?a=6&b=7:;/y:hi;/sub?a=10&b=4:;",
              "Expect request keeps its place between two others");
        check(buffer.empty(), "nothing left over");
    }
}

void test_incomplete_never_loses_bytes() {
    std::printf("6. any number of partial feeds still reassemble exactly\n");
    // Split a request carrying a body at every possible position, one byte per
    // feed, and require the result to be identical each time.
    const std::string request = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello world";
    for (std::size_t split = 1; split < request.size(); ++split) {
        std::string buffer = request.substr(0, split);
        check(http::take_one_request(buffer).status == http::FrameStatus::kIncomplete,
              "first fragment is incomplete");
        check(buffer == request.substr(0, split), "first fragment untouched");

        buffer += request.substr(split);
        const http::ParseResult r = http::take_one_request(buffer);
        check(r.status == http::FrameStatus::kComplete, "reassembled after the second half");
        check(r.error == http::ParseError::kNone, "reassembled request is valid");
        check(r.request.body == "hello world", "body reassembled exactly");
        check(buffer.empty(), "nothing left over");
    }
}

}  // namespace

int main() {
    test_truncated_request_line();
    test_truncated_body();
    test_oversized_body();
    test_head_never_completes();
    test_expect_100_continue();
    test_incomplete_never_loses_bytes();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
