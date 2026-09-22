// Unit tests for the framing layer: the receive-buffer invariant.
//
// These assert the property that recv() boundaries are irrelevant: for any
// split of the byte stream into chunks, parse_one_request must yield the same
// sequence of requests, in order, with no bytes lost or duplicated.

#include "http.h"

#include <cstdio>
#include <string>
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

std::string req(const std::string& target) {
    return "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// Feeds `data` to the framer one chunk at a time, collecting the head of every
// request produced. `chunks` gives the split points, i.e. exactly which bytes
// each simulated recv() would have returned.
std::vector<std::string> frame_by_chunks(const std::string& data,
                                         const std::vector<std::size_t>& chunks) {
    std::string buffer;
    std::vector<std::string> heads;

    std::size_t offset = 0;
    for (std::size_t n : chunks) {
        buffer.append(data, offset, n);
        offset += n;

        // Drain every request that is now complete.
        for (;;) {
            http::FramedRequest framed;
            const http::FrameStatus st = http::take_one_request(buffer, &framed);
            if (st != http::FrameStatus::kComplete) {
                break;
            }
            heads.push_back(framed.head);
        }
    }

    // Anything still buffered means the framer did not consume the whole input.
    if (!buffer.empty()) {
        std::printf("  FAIL: %zu bytes left unconsumed: \"%s\"\n", buffer.size(),
                    buffer.c_str());
        ++g_failures;
    }
    ++g_checks;
    return heads;
}

void expect_incomplete_then_complete() {
    std::printf("A. fragmented request across recv()\n");

    std::string buffer;
    http::FramedRequest framed;

    buffer.append("GET /add?a=2&b=3 HT");
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kIncomplete,
          "partial request line must report incomplete");
    check(buffer == "GET /add?a=2&b=3 HT", "buffer untouched while incomplete");

    buffer.append("TP/1.1\r\nHost: localhost\r\n");
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kIncomplete,
          "headers not terminated yet");

    buffer.append("\r\n");
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kComplete,
          "complete once CRLFCRLF arrives");
    check(framed.head == req("/add?a=2&b=3"), "head bytes exact");
    check(framed.head_length == req("/add?a=2&b=3").size(), "consumed length exact");
    check(buffer.empty(), "buffer empty after single request");
}

void expect_terminator_split_across_chunks() {
    std::printf("A2. CRLFCRLF itself split across recv()\n");

    const std::string full = req("/sub?a=1&b=1");
    for (std::size_t split = 1; split < full.size(); ++split) {
        std::string buffer;
        http::FramedRequest framed;

        buffer.append(full, 0, split);
        check(http::take_one_request(buffer, &framed) == http::FrameStatus::kIncomplete,
              "incomplete before terminator is fully buffered");

        buffer.append(full, split, std::string::npos);
        check(http::take_one_request(buffer, &framed) == http::FrameStatus::kComplete,
              "complete once remaining bytes arrive");
        check(framed.head == full, "reassembled head is byte-identical");
        check(buffer.empty(), "buffer drained");
    }
}

void expect_two_requests_one_buffer() {
    std::printf("B. two complete requests in a single recv()\n");

    const std::string r1 = req("/add?a=2&b=3");
    const std::string r2 = req("/mul?a=6&b=7");

    std::string buffer = r1 + r2;
    http::FramedRequest framed;

    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kComplete,
          "first request framed");
    check(framed.head == r1, "first request is the first one");
    check(buffer == r2, "second request preserved verbatim in buffer");

    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kComplete,
          "second request framed from leftover bytes alone");
    check(framed.head == r2, "second request intact");
    check(buffer.empty(), "buffer drained after both");
}

void expect_request_plus_partial_next() {
    std::printf("C. one request + first half of the next\n");

    const std::string r1 = req("/add?a=2&b=3");
    const std::string r2 = req("/mul?a=6&b=7");
    const std::string half = r2.substr(0, r2.size() / 2);

    std::string buffer = r1 + half;
    http::FramedRequest framed;

    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kComplete,
          "first request framed");
    check(framed.head == r1, "first request is exact");
    check(buffer == half, "partial next request preserved byte-for-byte");
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kIncomplete,
          "partial next request is not a request yet");
    check(buffer == half, "still untouched after incomplete");
}

void expect_pipelined_many() {
    std::printf("D. multiple pipelined requests, every split point\n");

    std::vector<std::string> expected;
    std::string all;
    const char* targets[] = {"/add?a=1&b=1", "/sub?a=2&b=2", "/mul?a=3&b=3",
                             "/div?a=9&b=3", "/add?a=4&b=4", "/sub?a=5&b=5"};
    for (const char* t : targets) {
        expected.push_back(req(t));
        all += req(t);
    }

    // One single recv() containing all six, split every possible way.
    for (std::size_t split = 1; split < all.size(); ++split) {
        const std::vector<std::string> heads = frame_by_chunks(all, {split, all.size() - split});
        check(heads.size() == expected.size(), "all six requests produced");
        bool order_ok = (heads.size() == expected.size());
        for (std::size_t i = 0; order_ok && i < heads.size(); ++i) {
            order_ok = (heads[i] == expected[i]);
        }
        check(order_ok, "responses come back in request order, bytes intact");
    }

    // One byte at a time: the worst possible fragmentation.
    std::vector<std::size_t> one_at_a_time(all.size(), 1);
    const std::vector<std::string> heads = frame_by_chunks(all, one_at_a_time);
    check(heads.size() == expected.size(), "byte-at-a-time yields all six");
    bool ok = (heads.size() == expected.size());
    for (std::size_t i = 0; ok && i < heads.size(); ++i) {
        ok = (heads[i] == expected[i]);
    }
    check(ok, "byte-at-a-time preserves order and content");

    // Single recv containing everything.
    const std::vector<std::string> all_at_once = frame_by_chunks(all, {all.size()});
    check(all_at_once.size() == expected.size(), "single recv yields all six");
}

void expect_no_bytes_lost() {
    std::printf("E. no bytes lost or duplicated over mixed fragmentation\n");

    std::string all;
    std::size_t total = 0;
    for (int i = 0; i < 20; ++i) {
        all += req("/add?a=1&b=2");
        total += req("/add?a=1&b=2").size();
    }

    // Ragged chunk sizes.
    const std::size_t sizes[] = {1, 7, 40, 3, 100, 2, 55, 9};
    std::vector<std::size_t> chunks;
    std::size_t offset = 0, i = 0;
    while (offset < all.size()) {
        std::size_t n = sizes[i++ % (sizeof(sizes) / sizeof(sizes[0]))];
        if (n > all.size() - offset) {
            n = all.size() - offset;
        }
        chunks.push_back(n);
        offset += n;
    }

    std::string buffer;
    std::vector<std::string> heads;
    std::size_t consumed = 0;
    offset = 0;
    for (std::size_t n : chunks) {
        buffer.append(all, offset, n);
        offset += n;
        for (;;) {
            http::FramedRequest framed;
            if (http::take_one_request(buffer, &framed) != http::FrameStatus::kComplete) {
                break;
            }
            heads.push_back(framed.head);
            consumed += framed.head_length;
        }
    }

    check(heads.size() == 20, "all 20 requests recovered");
    check(consumed == total, "consumed byte count equals bytes sent");
    check(buffer.empty(), "nothing left over");
    bool all_same = true;
    for (const std::string& h : heads) {
        if (h != req("/add?a=1&b=2")) {
            all_same = false;
        }
    }
    check(all_same, "every request byte-identical to what was sent");
}

void expect_head_too_large() {
    std::printf("F. oversized head cannot grow the buffer forever\n");

    std::string buffer(http::kMaxHeadBytes + 1, 'A');  // no CRLFCRLF anywhere
    http::FramedRequest framed;
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kHeadTooLarge,
          "reports kHeadTooLarge instead of buffering forever");

    // At the cap exactly, it is merely incomplete.
    std::string small(http::kMaxHeadBytes, 'A');
    check(http::take_one_request(small, &framed) == http::FrameStatus::kIncomplete,
          "exactly at the cap is still just incomplete");
}

void expect_empty_buffer() {
    std::printf("G. empty buffer\n");
    std::string buffer;
    http::FramedRequest framed;
    check(http::take_one_request(buffer, &framed) == http::FrameStatus::kIncomplete,
          "empty buffer is incomplete");
    check(buffer.empty(), "buffer still empty");
}

}  // namespace

int main() {
    expect_empty_buffer();
    expect_incomplete_then_complete();
    expect_terminator_split_across_chunks();
    expect_two_requests_one_buffer();
    expect_request_plus_partial_next();
    expect_pipelined_many();
    expect_no_bytes_lost();
    expect_head_too_large();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
