// Run: g++ -std=c++23 -Wall -Wextra -Werror test/test_response_order.cpp -o /tmp/test_response_order && /tmp/test_response_order
#include "../src/module/builtin/server/response_queue.hpp"
#include <cassert>

using zane::builtin::ResponseQueue;

int main() {
    ResponseQueue queue;
    std::string wire;
    auto writer = [&wire](const std::string& bytes) { wire += bytes; };

    // A rejection without an earlier pending request is sent immediately.
    auto immediate = queue.enqueue();
    queue.write(immediate, "503", true, writer);
    assert(wire == "503");
    wire.clear();

    auto first = queue.enqueue();
    auto second = queue.enqueue();
    auto rejected = queue.enqueue();
    queue.write(rejected, "503", true, writer);
    queue.write(second, "second", true, writer);
    assert(wire.empty());
    queue.write(first, "first", true, writer);
    assert(wire == "firstsecond503");
    wire.clear();

    // Headers and chunks may flush immediately, but a later response cannot
    // interleave with a stream before its terminating chunk.
    auto stream = queue.enqueue();
    auto later = queue.enqueue();
    queue.write(stream, "head|", false, writer);
    assert(wire == "head|");
    queue.write(later, "503", true, writer);
    queue.write(stream, "chunk|", false, writer);
    assert(wire == "head|chunk|");
    queue.write(stream, "end|", true, writer);
    assert(wire == "head|chunk|end|503");
    wire.clear();

    // A later stream can finish before the first response is ready.
    first = queue.enqueue();
    stream = queue.enqueue();
    queue.write(stream, "head|", false, writer);
    queue.write(stream, "end|", true, writer);
    assert(wire.empty());
    queue.write(first, "first|", true, writer);
    assert(wire == "first|head|end|");

    // Each connection owns an independent queue.
    queue.enqueue();
    ResponseQueue other;
    auto independent = other.enqueue();
    other.write(independent, "other", true, writer);
    assert(wire.ends_with("other"));
}
