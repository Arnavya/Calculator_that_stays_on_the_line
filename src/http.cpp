#include "http.h"

namespace http {
namespace {

const char kHeaderTerminator[] = "\r\n\r\n";
constexpr std::size_t kHeaderTerminatorLength = 4;

}  // namespace

FrameStatus take_one_request(std::string& buffer, FramedRequest* out) {
    const std::size_t term = buffer.find(kHeaderTerminator, 0,
                                         sizeof(kHeaderTerminator) - 1);
    if (term == std::string::npos) {
        // No terminator yet. If the buffered bytes already exceed the cap, no
        // future bytes can rescue this head, so report it instead of growing
        // the buffer forever.
        if (buffer.size() > kMaxHeadBytes) {
            return FrameStatus::kHeadTooLarge;
        }
        return FrameStatus::kIncomplete;
    }

    const std::size_t head_length = term + kHeaderTerminatorLength;

    if (out != nullptr) {
        out->head.assign(buffer, 0, head_length);
        out->head_length = head_length;
    }

    // Remove exactly this request. Anything after head_length is a later
    // request (or the head of one) and must survive.
    buffer.erase(0, head_length);

    return FrameStatus::kComplete;
}

}  // namespace http
