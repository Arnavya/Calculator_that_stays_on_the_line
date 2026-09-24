// Unit tests for calculator routing, validation, and arithmetic.

#include "calculator.h"

#include <climits>
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

http::Request get(const std::string& target) {
    http::Request request;
    request.method = "GET";
    request.target = target;
    request.version = "HTTP/1.1";
    request.headers.emplace_back("host", "localhost");
    return request;
}

// Asserts status and exact body for a GET target.
void expect(const std::string& target, int status, const char* body) {
    const http::Response response = calc::handle(get(target));
    if (response.status_code != status || response.body != body) {
        ++g_failures;
        std::printf("  FAIL: GET %s -> %d \"%s\" (want %d \"%s\")\n",
                    target.c_str(), response.status_code, response.body.c_str(),
                    status, body);
    }
    ++g_checks;
}

// The required acceptance matrix, exactly.
void test_acceptance_matrix() {
    std::printf("1. required acceptance matrix\n");
    expect("/add?a=2&b=3", 200, "5");
    expect("/sub?a=10&b=4", 200, "6");
    expect("/mul?a=6&b=7", 200, "42");
    expect("/div?a=9&b=3", 200, "3");
    expect("/div?a=1&b=0", 400, "Bad Request");
    expect("/add?a=x&b=3", 400, "Bad Request: invalid a");
    expect("/pow?a=2&b=8", 404, "Not Found");
}

// The spec's /div?a=10&b=4 example is a typo; normal integer division is 2.
void test_division_is_normal_integer_division() {
    std::printf("2. integer division, not the spec's typo\n");
    expect("/div?a=10&b=4", 200, "2");
    expect("/div?a=1&b=2", 200, "0");
    expect("/div?a=7&b=2", 200, "3");
    expect("/div?a=-7&b=2", 200, "-3");   // truncates toward zero
    expect("/div?a=7&b=-2", 200, "-3");
    expect("/div?a=0&b=5", 200, "0");
    expect("/div?a=5&b=1", 200, "5");
}

void test_arithmetic() {
    std::printf("3. arithmetic\n");
    expect("/add?a=0&b=0", 200, "0");
    expect("/add?a=-5&b=3", 200, "-2");
    expect("/add?a=1000000&b=2000000", 200, "3000000");
    expect("/sub?a=3&b=10", 200, "-7");
    expect("/sub?a=10&b=10", 200, "0");
    expect("/mul?a=0&b=999", 200, "0");
    expect("/mul?a=-6&b=7", 200, "-42");
    expect("/mul?a=-6&b=-7", 200, "42");
    expect("/mul?a=100000&b=100000", 200, "10000000000");
}

void test_missing_parameters() {
    std::printf("4. missing parameters -> 400\n");
    expect("/add", 400, "Bad Request: invalid a");
    expect("/add?a=2", 400, "Bad Request: invalid b");
    expect("/add?b=3", 400, "Bad Request: invalid a");
    expect("/add?a=&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=2&b=", 400, "Bad Request: invalid b");
    expect("/add?", 400, "Bad Request: invalid a");
    expect("/add?a", 400, "Bad Request: invalid a");
    expect("/add?a=2&", 400, "Bad Request: invalid b");
    expect("/add?a=2&b", 400, "Bad Request: invalid b");
    expect("/div?a=1", 400, "Bad Request: invalid b");
}

void test_invalid_integers() {
    std::printf("5. invalid integers -> 400\n");
    expect("/add?a=x&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=2&b=y", 400, "Bad Request: invalid b");
    expect("/add?a=2.5&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=+2&b=3", 400, "Bad Request: invalid a");
    expect("/add?a= 2&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=2 &b=3", 400, "Bad Request: invalid a");
    expect("/add?a=0x10&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=1e3&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=--1&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=-&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=1,000&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=99999999999999999999999&b=3", 400, "Bad Request: invalid a");
    expect("/add?a=2&b=99999999999999999999999", 400, "Bad Request: invalid b");
}

void test_routing() {
    std::printf("6. routing\n");
    expect("/pow?a=2&b=8", 404, "Not Found");
    expect("/", 404, "Not Found");
    expect("/addx?a=2&b=3", 404, "Not Found");
    expect("/ADD?a=2&b=3", 404, "Not Found");     // paths are case-sensitive
    expect("/add/?a=2&b=3", 404, "Not Found");
    expect("/add/extra?a=2&b=3", 404, "Not Found");
    expect("/mod?a=5&b=2", 404, "Not Found");     // only four routes exist
    expect("/div2?a=5&b=2", 404, "Not Found");
    // A 404 wins over bad parameters: the route does not exist at all.
    expect("/pow?a=x&b=y", 404, "Not Found");
}

void test_method() {
    std::printf("7. non-GET -> 405\n");
    const char* methods[] = {"POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS", "get"};
    for (const char* m : methods) {
        http::Request request = get("/add?a=2&b=3");
        request.method = m;
        const http::Response response = calc::handle(request);
        if (response.status_code != 405) {
            ++g_failures;
            std::printf("  FAIL: %s /add?a=2&b=3 -> %d (want 405)\n", m,
                        response.status_code);
        }
        ++g_checks;
    }
    // 405 wins over both 404 and 400: the method is checked first.
    http::Request post = get("/add?a=2&b=3");
    post.method = "POST";
    check(calc::handle(post).status_code == 405, "POST /add -> 405");

    http::Request post_unknown = get("/pow?a=2&b=8");
    post_unknown.method = "POST";
    check(calc::handle(post_unknown).status_code == 405, "method beats path");

    http::Request post_noquery = get("/add");
    post_noquery.method = "POST";
    check(calc::handle(post_noquery).status_code == 405, "method beats params");
}

void test_parameter_order_and_extras() {
    std::printf("8. parameter order and extras\n");
    expect("/add?b=3&a=2", 200, "5");           // order does not matter
    expect("/add?a=2&b=3&c=9", 200, "5");       // unknown params ignored
    expect("/add?c=9&a=2&b=3", 200, "5");
    expect("/add?a=2&a=100&b=3", 200, "5");     // first occurrence wins
    expect("/add?a=1&&b=2", 200, "3");          // empty pair ignored
    expect("/add?=2&b=3", 400, "Bad Request: invalid a");  // no key
}

void test_overflow_is_not_undefined() {
    std::printf("9. overflow and the trapping division\n");
    long long out = 0;
    check(!calc::compute(calc::Op::kAdd, LLONG_MAX, 1, &out),
          "LLONG_MAX + 1 rejected, not wrapped");
    check(!calc::compute(calc::Op::kAdd, LLONG_MIN, -1, &out),
          "LLONG_MIN + -1 rejected");
    check(!calc::compute(calc::Op::kSub, LLONG_MIN, 1, &out),
          "LLONG_MIN - 1 rejected");
    check(!calc::compute(calc::Op::kMul, LLONG_MAX, 2, &out),
          "LLONG_MAX * 2 rejected");
    check(!calc::compute(calc::Op::kDiv, 1, 0, &out), "division by zero rejected");
    check(!calc::compute(calc::Op::kDiv, LLONG_MIN, -1, &out),
          "LLONG_MIN / -1 rejected (would trap)");

    check(calc::compute(calc::Op::kAdd, LLONG_MAX - 1, 1, &out) && out == LLONG_MAX,
          "largest representable sum accepted");
    check(calc::compute(calc::Op::kMul, LLONG_MIN, 1, &out) && out == LLONG_MIN,
          "LLONG_MIN * 1 accepted");
    check(calc::compute(calc::Op::kDiv, LLONG_MIN, 1, &out) && out == LLONG_MIN,
          "LLONG_MIN / 1 accepted");

    expect("/add?a=9223372036854775807&b=1", 400, "Bad Request");
    expect("/mul?a=9223372036854775807&b=2", 400, "Bad Request");
    expect("/div?a=-9223372036854775808&b=-1", 400, "Bad Request");
    expect("/add?a=-9223372036854775808&b=0", 200, "-9223372036854775808");
}

void test_parse_int() {
    std::printf("10. parse_int\n");
    long long v = 0;
    check(calc::parse_int("0", &v) && v == 0, "\"0\"");
    check(calc::parse_int("42", &v) && v == 42, "\"42\"");
    check(calc::parse_int("-42", &v) && v == -42, "\"-42\"");
    check(calc::parse_int("007", &v) && v == 7, "leading zeros accepted");
    check(calc::parse_int("9223372036854775807", &v) && v == LLONG_MAX, "LLONG_MAX");
    check(calc::parse_int("-9223372036854775808", &v) && v == LLONG_MIN, "LLONG_MIN");
    check(!calc::parse_int("9223372036854775808", &v), "LLONG_MAX + 1 rejected");
    check(!calc::parse_int("-9223372036854775809", &v), "LLONG_MIN - 1 rejected");
    check(!calc::parse_int("", &v), "empty rejected");
    check(!calc::parse_int("-", &v), "lone minus rejected");
    check(!calc::parse_int("+1", &v), "plus rejected");
    check(!calc::parse_int(" 1", &v), "leading space rejected");
    check(!calc::parse_int("1 ", &v), "trailing space rejected");
    check(!calc::parse_int("1a", &v), "trailing junk rejected");
    check(!calc::parse_int("0x1f", &v), "hex rejected");
}

void test_successful_body_has_no_trailing_newline() {
    std::printf("11. successful body format\n");
    const http::Response response = calc::handle(get("/add?a=2&b=3"));
    check(response.body == "5", "body is exactly \"5\"");
    check(response.body.find('\n') == std::string::npos, "no newline in body");
    check(response.body.back() == '5', "no trailing whitespace");
    check(response.close_after == false, "success stays persistent");
}

}  // namespace

int main() {
    test_acceptance_matrix();
    test_division_is_normal_integer_division();
    test_arithmetic();
    test_missing_parameters();
    test_invalid_integers();
    test_routing();
    test_method();
    test_parameter_order_and_extras();
    test_overflow_is_not_undefined();
    test_parse_int();
    test_successful_body_has_no_trailing_newline();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
