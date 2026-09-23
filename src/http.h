#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace http {

// Upper bound on the request head (request line + headers + trailing CRLFCRLF).
// Without this a peer could stream junk forever and grow the buffer without
// limit. Exceeding it means the head can never become valid.
inline constexpr std::size_t kMaxHeadBytes = 64 * 1024;

// Upper bound on a request body. Calculator requests have no body, but the
// framing logic must understand Content-Length, so the bound has to exist.
inline constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

enum class FrameStatus {
    kIncomplete,    // not enough bytes buffered yet; recv() again
    kComplete,     // one whole request is available and was consumed
    kHeadTooLarge,  // head exceeded kMaxHeadBytes and can never complete
};

enum class ParseError {
    kNone = 0,
    kMalformedRequestLine,
    kUnsupportedVersion,
    kMalformedHeader,
    kMissingHost,
    kDuplicateHeader,
    kInvalidContentLength,
    kUnsupportedTransferEncoding,
    kHeadTooLarge,
};

const char* describe(ParseError error);

struct Request {
    std::string method;
    std::string target;
    std::string version;
    // Header names are lowercased so lookup is case-insensitive, as HTTP
    // requires. Lookup by exact (lowercased) name.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::size_t head_length = 0;
    std::size_t body_length = 0;

    // `name` must already be lowercase.
    bool has_header(const std::string& name) const;
    std::string header(const std::string& name) const;
};

struct ParseResult {
    FrameStatus status = FrameStatus::kIncomplete;
    ParseError error = ParseError::kNone;
    // True when the extent of this request could not be determined, so the
    // byte stream cannot be resynchronised. The caller must send its response
    // and then close. False for every error where the next request's start is
    // known exactly (e.g. a missing Host).
    bool framing_lost = false;
    // Populated when status == kComplete. On error, whatever was parsed before
    // the error is present.
    Request request;
};

// Consumes AT MOST one request from the front of `buffer`.
//
// kComplete: `buffer` is advanced past exactly this request -- head plus, when
//            Content-Length was present and valid, exactly that many body
//            bytes. Bytes belonging to any later request stay in `buffer`,
//            untouched and in order.
// kIncomplete: `buffer` is left exactly as it was, even if a head was already
//              found -- a body announced by Content-Length may still be short.
//
// The head boundary is the CRLFCRLF ending the header block; the body boundary
// is Content-Length. Socket EOF is never consulted: recv() boundaries carry no
// HTTP meaning, and neither does the peer closing the connection.
ParseResult take_one_request(std::string& buffer);

}  // namespace http
