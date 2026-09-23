#include "http.h"

#include <limits>

namespace http {
namespace {

const char kHeaderTerminator[] = "\r\n\r\n";
constexpr std::size_t kHeaderTerminatorLength = 4;

// RFC 9110 tchar: the set of characters allowed in a method or header name.
bool is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_ows(char c) { return c == ' ' || c == '\t'; }

// Values may hold HTAB, printable ASCII, and obs-text. Any other control
// character means the peer sent something that is not a header value.
bool is_valid_header_value_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == '\t') {
        return true;
    }
    if (u < 0x20 || u == 0x7f) {
        return false;
    }
    return true;
}

void to_lowercase(std::string* text) {
    for (char& c : *text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
}

std::string trim_ows(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_ows(text[begin])) {
        ++begin;
    }
    while (end > begin && is_ows(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

ParseError parse_request_line(const std::string& line, Request* out) {
    // METHOD SP TARGET SP VERSION -- exactly two single spaces.
    const std::size_t first = line.find(' ');
    if (first == std::string::npos || first == 0) {
        return ParseError::kMalformedRequestLine;
    }
    const std::size_t second = line.find(' ', first + 1);
    if (second == std::string::npos) {
        return ParseError::kMalformedRequestLine;
    }
    if (line.find(' ', second + 1) != std::string::npos) {
        return ParseError::kMalformedRequestLine;
    }

    out->method = line.substr(0, first);
    out->target = line.substr(first + 1, second - first - 1);
    out->version = line.substr(second + 1);

    if (out->method.empty() || out->target.empty() || out->version.empty()) {
        return ParseError::kMalformedRequestLine;
    }
    for (char c : out->method) {
        if (!is_tchar(c)) {
            return ParseError::kMalformedRequestLine;
        }
    }
    for (char c : out->target) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) {
            return ParseError::kMalformedRequestLine;
        }
    }
    // Origin-form only ("/path?query"). This is not a general-purpose HTTP
    // server, so absolute-form and authority-form targets are rejected.
    if (out->target[0] != '/') {
        return ParseError::kMalformedRequestLine;
    }
    if (out->version != "HTTP/1.1") {
        return ParseError::kUnsupportedVersion;
    }
    return ParseError::kNone;
}

ParseError add_header(const std::string& line, Request* out) {
    if (line.empty()) {
        return ParseError::kMalformedHeader;
    }
    // Obsolete line folding (a continuation line) is not supported.
    if (is_ows(line[0])) {
        return ParseError::kMalformedHeader;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return ParseError::kMalformedHeader;
    }

    // A header name must be a non-empty token. Because SPACE and HTAB are not
    // tchars, this also rejects the obsolete "Name : value" form.
    std::string name = line.substr(0, colon);
    if (name.empty()) {
        return ParseError::kMalformedHeader;
    }
    for (char c : name) {
        if (!is_tchar(c)) {
            return ParseError::kMalformedHeader;
        }
    }

    std::string value = trim_ows(line.substr(colon + 1));
    for (char c : value) {
        if (!is_valid_header_value_char(c)) {
            return ParseError::kMalformedHeader;
        }
    }

    to_lowercase(&name);

    // Repeated Host or Content-Length is rejected: both are single-instance
    // and a disagreement between duplicates would make framing ambiguous.
    if ((name == "host" || name == "content-length") && out->has_header(name)) {
        return ParseError::kDuplicateHeader;
    }

    out->headers.emplace_back(std::move(name), std::move(value));
    return ParseError::kNone;
}

// `terminator_offset` is the index of the CRLFCRLF that ends the header block,
// relative to the start of `head`.
ParseError parse_head(const std::string& head, std::size_t terminator_offset,
                      Request* out) {
    const std::size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) {
        return ParseError::kMalformedRequestLine;
    }

    const ParseError line_error =
        parse_request_line(head.substr(0, line_end), out);
    if (line_error != ParseError::kNone) {
        return line_error;
    }

    // Header lines live between the request line's CRLF and the CRLF that
    // begins the terminator. When there are no headers the terminator starts
    // at the same offset as the request line's CRLF, so there is nothing to
    // read; the guard also keeps the length from going negative.
    const std::size_t headers_begin = line_end + 2;
    if (terminator_offset > headers_begin) {
        std::string block = head.substr(headers_begin,
                                        terminator_offset - headers_begin);
        std::size_t pos = 0;
        while (pos < block.size()) {
            const std::size_t eol = block.find("\r\n", pos);
            const std::size_t len =
                (eol == std::string::npos) ? block.size() - pos : eol - pos;
            const ParseError error = add_header(block.substr(pos, len), out);
            if (error != ParseError::kNone) {
                return error;
            }
            if (eol == std::string::npos) {
                break;
            }
            pos = eol + 2;
        }
    }

    // HTTP/1.1 requires exactly one Host. Its value is not restricted.
    if (!out->has_header("host")) {
        return ParseError::kMissingHost;
    }
    return ParseError::kNone;
}

bool parse_content_length(const std::string& text, std::size_t* out) {
    if (text.empty()) {
        return false;
    }
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    std::size_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            // Rejects signs, whitespace, hex, and comma-separated lists.
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (value > (limit - digit) / 10) {
            return false;  // overflow
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

}  // namespace

const char* describe(ParseError error) {
    switch (error) {
        case ParseError::kNone:
            return "ok";
        case ParseError::kMalformedRequestLine:
            return "malformed request line";
        case ParseError::kUnsupportedVersion:
            return "unsupported HTTP version";
        case ParseError::kMalformedHeader:
            return "malformed header";
        case ParseError::kMissingHost:
            return "missing Host header";
        case ParseError::kDuplicateHeader:
            return "duplicate Host or Content-Length header";
        case ParseError::kInvalidContentLength:
            return "invalid Content-Length";
        case ParseError::kUnsupportedTransferEncoding:
            return "Transfer-Encoding is not supported";
        case ParseError::kHeadTooLarge:
            return "request head too large";
    }
    return "unknown error";
}

bool Request::has_header(const std::string& name) const {
    for (const auto& entry : headers) {
        if (entry.first == name) {
            return true;
        }
    }
    return false;
}

std::string Request::header(const std::string& name) const {
    for (const auto& entry : headers) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return std::string();
}

ParseResult take_one_request(std::string& buffer) {
    ParseResult result;

    const std::size_t term =
        buffer.find(kHeaderTerminator, 0, kHeaderTerminatorLength);
    if (term == std::string::npos) {
        // No terminator yet. If the buffered bytes already exceed the cap, no
        // future bytes can rescue this head, so report it instead of growing
        // the buffer forever.
        if (buffer.size() > kMaxHeadBytes) {
            result.status = FrameStatus::kHeadTooLarge;
            result.error = ParseError::kHeadTooLarge;
            result.framing_lost = true;
            return result;
        }
        result.status = FrameStatus::kIncomplete;
        return result;
    }

    const std::size_t head_length = term + kHeaderTerminatorLength;

    Request request;
    request.head_length = head_length;

    ParseError error = parse_head(buffer.substr(0, head_length), term, &request);

    // Work out the body extent. This is only meaningful once the head itself
    // parsed, because a broken head means we cannot trust its headers.
    bool extent_known = true;
    std::size_t body_length = 0;

    if (error == ParseError::kNone) {
        if (request.has_header("transfer-encoding")) {
            // Chunked framing is deliberately not implemented, so the body
            // length is unknowable and the stream cannot be resynchronised.
            error = ParseError::kUnsupportedTransferEncoding;
            extent_known = false;
        } else if (request.has_header("content-length")) {
            std::size_t declared = 0;
            if (!parse_content_length(request.header("content-length"), &declared) ||
                declared > kMaxBodyBytes) {
                error = ParseError::kInvalidContentLength;
                extent_known = false;
            } else {
                body_length = declared;
            }
        }
    }

    const std::size_t total = head_length + body_length;
    if (buffer.size() < total) {
        // The head is here but the announced body is still arriving. Leave the
        // buffer completely untouched and wait for more bytes.
        result.status = FrameStatus::kIncomplete;
        return result;
    }

    if (body_length > 0) {
        request.body.assign(buffer, head_length, body_length);
    }
    request.body_length = body_length;

    // Advance past exactly this request. Everything from `total` onward belongs
    // to a later request and must survive untouched.
    buffer.erase(0, total);

    result.status = FrameStatus::kComplete;
    result.error = error;
    result.framing_lost = !extent_known;
    result.request = std::move(request);
    return result;
}

}  // namespace http
