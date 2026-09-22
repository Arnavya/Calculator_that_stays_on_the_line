#pragma once

#include <cstddef>
#include <string>

namespace http {

// Upper bound on the request head (request line + headers + trailing CRLFCRLF).
// Without this a peer could stream junk forever and grow the buffer without
// limit. Exceeding it means the head can never become valid.
inline constexpr std::size_t kMaxHeadBytes = 64 * 1024;

enum class FrameStatus {
    kIncomplete,    // not enough bytes buffered yet; recv() again
    kComplete,      // exactly one request head is available
    kHeadTooLarge,  // head exceeded kMaxHeadBytes and can never complete
};

struct FramedRequest {
    // Request head including the terminating CRLFCRLF. STEP 4 parses this into
    // method / target / version / headers; here it is only the byte range.
    std::string head;
    // Number of bytes this request occupied at the front of the buffer.
    std::size_t head_length = 0;
};

// Consumes AT MOST one request head from the front of `buffer`.
//
// kComplete: `out` receives the head and `buffer` is erased by exactly
//            head_length bytes. Bytes belonging to any later request stay in
//            `buffer`, untouched, in their original order.
// kIncomplete: `buffer` is left exactly as it was.
//
// A request boundary is the CRLFCRLF that ends the header block. Socket EOF is
// never consulted: recv() boundaries carry no HTTP meaning.
FrameStatus take_one_request(std::string& buffer, FramedRequest* out);

}  // namespace http
