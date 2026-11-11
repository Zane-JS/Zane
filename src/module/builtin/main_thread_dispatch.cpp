#include "main_thread_dispatch.hpp"

#include "task_queue.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <mutex>

namespace zane {
namespace builtin {

namespace {

using Thunk = std::function<void(v8::Isolate*, v8::Local<v8::Context>)>;

std::mutex& queueMutex() {
    static std::mutex s_mutex;
    return s_mutex;
}

std::deque<Thunk>& queue() {
    static std::deque<Thunk> s_queue;
    return s_queue;
}

// The isolate that owns the event loop. Only written by the main thread.
// Read from background threads, hence atomic.
std::atomic<v8::Isolate*> g_main_isolate{nullptr};

constexpr int32_t kMaxTasksPerDrain = 1024;

} // namespace

void setMainThreadLoop(v8::Isolate* p_isolate, v8::Local<v8::Context> /*context*/) {
    g_main_isolate.store(p_isolate, std::memory_order_release);

    // Shutting down: the loop will never run again, so drop anything still
    // queued. Destroying the thunks releases the Request/Response objects they
    // captured instead of leaking them for the lifetime of the process.
    if (!p_isolate) {
        std::deque<Thunk> dropped;
        {
            std::lock_guard<std::mutex> lock(queueMutex());
            dropped.swap(queue());
        }
        // `dropped` is destroyed here, outside the lock.
    }
}

bool hasMainThreadLoop() {
    return g_main_isolate.load(std::memory_order_acquire) != nullptr;
}

bool postToMainThread(Thunk fn) {
    if (!hasMainThreadLoop()) return false;
    {
        std::lock_guard<std::mutex> lock(queueMutex());
        queue().push_back(std::move(fn));
    }
    // Wake the loop: it may be parked in TaskQueue::wait() for up to its poll
    // timeout. Without this a request would sit until the next timer tick.
    zane::TaskQueue::getInstance().notify();
    return true;
}

size_t pendingMainThreadTasks() {
    std::lock_guard<std::mutex> lock(queueMutex());
    return queue().size();
}

void drainMainThreadTasks(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    if (!p_isolate) return;

    // Bounded batch: an HTTP request handler that throws or re-posts must not
    // be able to starve timers and the rest of the loop.
    for (int32_t i = 0; i < kMaxTasksPerDrain; ++i) {
        Thunk fn;
        {
            std::lock_guard<std::mutex> lock(queueMutex());
            if (queue().empty()) return;
            fn = std::move(queue().front());
            queue().pop_front();
        }

        // Each thunk gets a fresh handle scope so the locals it creates are
        // collected as soon as it returns, and any exception it leaves pending
        // is caught here rather than escaping into the runtime.
        v8::HandleScope handle_scope(p_isolate);
        v8::Context::Scope context_scope(context);
        v8::TryCatch try_catch(p_isolate);

        fn(p_isolate, context);

        if (try_catch.HasCaught()) {
            v8::Local<v8::Value> exception = try_catch.Exception();
            v8::String::Utf8Value message(p_isolate, exception);
            std::cerr << "Uncaught exception in background callback: "
                      << (*message ? *message : "unknown") << std::endl;
        }
    }
}

} // namespace builtin
} // namespace zane
