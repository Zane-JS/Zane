// Run: g++ -std=c++23 -Wall -Wextra -Werror test/test_response_order.cpp -o /tmp/test_response_order && /tmp/test_response_order
#include "../src/module/builtin/server/response_queue.hpp"
#include <cassert>

using zane::builtin::ResponseQueue;

int main() {
    ResponseQueue queue;
    std::string wire;
    bool closed = false;
    auto writer = [&wire, &closed](const std::string& bytes, bool close) {
        assert(!closed);
        wire += bytes;
        closed = close;
    };

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

    // A queued rejection closes only after the earlier stream terminates.
    ResponseQueue close_queue;
    wire.clear();
    auto open_stream = close_queue.enqueue();
    auto rejection = close_queue.enqueue();
    close_queue.closeAfter(rejection);
    assert(!close_queue.enqueue());
    close_queue.write(rejection, "503", true, writer);
    close_queue.write(open_stream, "head|", false, writer);
    assert(wire == "head|" && !closed);
    close_queue.write(open_stream, "end|", true, writer);
    assert(wire == "head|end|503" && closed);

    // A rejection in the front slot shuts down immediately after its bytes.
    ResponseQueue immediate_close;
    wire.clear();
    closed = false;
    auto only_rejection = immediate_close.enqueue();
    immediate_close.closeAfter(only_rejection);
    immediate_close.write(only_rejection, "503", true, writer);
    assert(wire == "503" && closed);

    // Account across slots and chunks, including the exact boundary.
    ResponseQueue bounded;
    auto blocked = bounded.enqueue();
    auto waiting = bounded.enqueue();
    auto another = bounded.enqueue();
    auto discard = [](const std::string&, bool) {};
    assert(bounded.write(waiting, std::string(ResponseQueue::kMaxBufferedBytes - 1, 'x'), false, discard));
    assert(bounded.write(another, "y", true, discard));
    assert(!bounded.write(waiting, "z", true, discard));
    assert(bounded.closing());
    assert(bounded.write(blocked, "ignored", true, discard));
    assert(waiting->m_bytes.empty() && another->m_bytes.empty());

    // Completion releases queued bytes for subsequent waiting responses.
    ResponseQueue reuse;
    auto first_reuse = reuse.enqueue();
    auto second_reuse = reuse.enqueue();
    assert(reuse.write(second_reuse, std::string(ResponseQueue::kMaxBufferedBytes, 'x'), false, discard));
    assert(reuse.write(first_reuse, "done", true, discard));
    auto third_reuse = reuse.enqueue();
    assert(reuse.write(third_reuse, std::string(ResponseQueue::kMaxBufferedBytes, 'y'), true, discard));
    assert(reuse.write(second_reuse, "end", true, discard));

    // Direct writes remain unrestricted by the buffered-response limit.
    ResponseQueue direct;
    auto current = direct.enqueue();
    assert(direct.write(current, std::string(ResponseQueue::kMaxBufferedBytes + 1, 'x'), true, discard));

    ResponseQueue slots;
    auto first_slot = slots.enqueue();
    for (size_t i = 1; i < ResponseQueue::kMaxPendingResponses; ++i) assert(slots.enqueue());
    assert(!slots.enqueue());
    assert(slots.write(first_slot, "done", true, discard));
    assert(slots.enqueue());
    slots.abort();
    assert(!slots.enqueue());
}
