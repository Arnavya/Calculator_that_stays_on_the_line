#include "calculator.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

namespace calc {
namespace {

http::Response text_response(int status_code, const std::string& body) {
    http::Response response;
    response.status_code = status_code;
    response.body = body;
    return response;
}

// Splits "/add?a=2&b=3" into "/add" and "a=2&b=3". A target with no '?' yields an
// empty query, which then fails validation as a missing parameter.
void split_target(const std::string& target, std::string* path,
                  std::string* query) {
    const std::size_t mark = target.find('?');
    if (mark == std::string::npos) {
        *path = target;
        query->clear();
        return;
    }
    path->assign(target, 0, mark);
    query->assign(target, mark + 1, std::string::npos);
}

// Extracts the raw a and b values. First occurrence wins, so a repeated
// parameter is deterministic rather than order-dependent. Unrelated parameters
// are ignored. A pair with no '=' is not a parameter at all.
void find_params(const std::string& query, std::string* a, std::string* b) {
    bool have_a = false;
    bool have_b = false;
    std::size_t pos = 0;

    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::size_t len =
            (amp == std::string::npos) ? query.size() - pos : amp - pos;
        const std::string pair = query.substr(pos, len);

        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            const std::string key = pair.substr(0, eq);
            const std::string value = pair.substr(eq + 1);
            if (key == "a" && !have_a) {
                *a = value;
                have_a = true;
            } else if (key == "b" && !have_b) {
                *b = value;
                have_b = true;
            }
        }

        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
}

bool op_for_path(const std::string& path, Op* out) {
    if (path == "/add") {
        *out = Op::kAdd;
        return true;
    }
    if (path == "/sub") {
        *out = Op::kSub;
        return true;
    }
    if (path == "/mul") {
        *out = Op::kMul;
        return true;
    }
    if (path == "/div") {
        *out = Op::kDiv;
        return true;
    }
    return false;
}

}  // namespace

bool parse_int(const std::string& text, long long* out) {
    if (text.empty()) {
        return false;
    }

    // Validate the character set before converting, so strtoll's tolerance for
    // leading whitespace, a '+' sign, and other bases cannot leak through.
    std::size_t i = (text[0] == '-') ? 1 : 0;
    if (i >= text.size()) {
        return false;  // a lone "-"
    }
    for (std::size_t k = i; k < text.size(); ++k) {
        if (text[k] < '0' || text[k] > '9') {
            return false;
        }
    }

    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size()) {
        return false;  // out of range for long long
    }
    *out = value;
    return true;
}

bool compute(Op op, long long a, long long b, long long* out) {
    switch (op) {
        case Op::kAdd:
            // The builtins report overflow instead of wrapping silently, which
            // keeps signed overflow undefined behaviour out of this program.
            return !__builtin_add_overflow(a, b, out);
        case Op::kSub:
            return !__builtin_sub_overflow(a, b, out);
        case Op::kMul:
            return !__builtin_mul_overflow(a, b, out);
        case Op::kDiv:
            if (b == 0) {
                return false;
            }
            if (a == LLONG_MIN && b == -1) {
                return false;  // the one division that traps
            }
            *out = a / b;  // C++ truncates toward zero
            return true;
    }
    return false;
}

http::Response handle(const http::Request& request) {
    // Method is checked first: POST /add must be 405, not 400 for a missing
    // query string.
    if (request.method != "GET") {
        return text_response(http::kStatusMethodNotAllowed,
                             "Method Not Allowed");
    }

    std::string path;
    std::string query;
    split_target(request.target, &path, &query);

    Op op = Op::kAdd;
    if (!op_for_path(path, &op)) {
        return text_response(http::kStatusNotFound, "Not Found");
    }

    std::string a_text;
    std::string b_text;
    find_params(query, &a_text, &b_text);

    long long a = 0;
    if (!parse_int(a_text, &a)) {
        return text_response(http::kStatusBadRequest, "Bad Request: invalid a");
    }
    long long b = 0;
    if (!parse_int(b_text, &b)) {
        return text_response(http::kStatusBadRequest, "Bad Request: invalid b");
    }

    long long result = 0;
    if (!compute(op, a, b, &result)) {
        return text_response(http::kStatusBadRequest, "Bad Request");
    }

    return text_response(http::kStatusOk, std::to_string(result));
}

}  // namespace calc
