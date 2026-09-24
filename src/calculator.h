#pragma once

#include "http.h"
#include "response.h"

namespace calc {

// Parses a base-10 integer. Accepts an optional leading '-' followed by one or
// more digits, and nothing else: no leading/trailing whitespace, no '+', no
// hex, no exponent. Returns false for an empty string or a value that does not
// fit in long long.
bool parse_int(const std::string& text, long long* out);

enum class Op {
    kAdd,
    kSub,
    kMul,
    kDiv,
};

// Applies `op`. Returns false when the result cannot be represented: division
// by zero, signed overflow, and the one division that traps (LLONG_MIN / -1).
bool compute(Op op, long long a, long long b, long long* out);

// Routes a parsed request and produces the response.
//
// Exactly four routes exist, all GET only:
//     /add?a=<int>&b=<int>   a + b
//     /sub?a=<int>&b=<int>   a - b
//     /mul?a=<int>&b=<int>   a * b
//     /div?a=<int>&b=<int>   a / b   (integer division, truncating toward zero)
//
// Status selection order is fixed:
//     non-GET method            -> 405
//     path outside the four     -> 404
//     missing/invalid a or b    -> 400
//     division by zero          -> 400
//     result not representable  -> 400
// otherwise 200 with the decimal result as the whole body, no trailing newline.
http::Response handle(const http::Request& request);

}  // namespace calc
