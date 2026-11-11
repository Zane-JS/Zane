#ifndef ZANE_BUILTIN_MAIN_THREAD_DISPATCH_H
#define ZANE_BUILTIN_MAIN_THREAD_DISPATCH_H

#include "v8.h"
#include <atomic>
#include <cstddef>
#include <functional>

namespace zane {
namespace builtin {

// ============================================================================
// Main-thread dispatch for background (I/O) threads
// ============================================================================
//
// V8 is single threaded: an isolate may only be entered from one thread at a
// time, and every JS-visible side effect (timers, microtasks, promise
// reactions, async_hooks, Buffer/Uint8Array allocation) is queued on the thread
// that drives the event loop. Background threads — Trantor's TcpServer loop in
// particular — must therefore never touch the isolate directly. They post a
// thunk here instead and the main loop runs it, in order, between timer ticks.
//
// The loop state is a single global: Zane is a one-isolate-per-process runtime.

// Called by the runtime each iteration of its event loop. Passing a null
// isolate (when the loop exits) disables dispatch: posted work is then dropped
// instead of leaking or running without a HandleScope.
void setMainThreadLoop(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

// True when a main thread is currently draining this queue.
bool hasMainThreadLoop();

// Queue `fn` to run on the main thread. Safe to call from any thread.
// The thunk runs inside a HandleScope with the stored context entered, so it
// may create and use Local handles. Returns false if no main thread loop is
// active (the runtime is shutting down) — the thunk is dropped, not run.
bool postToMainThread(std::function<void(v8::Isolate*, v8::Local<v8::Context>)> fn);

// Run the queued thunks on the calling (main) thread. Called once per event
// loop iteration, before timers, so I/O callbacks are ordered ahead of the
// work they may schedule.
void drainMainThreadTasks(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

// Number of thunks queued but not yet executed. Used by the loop's liveness
// check so pending callbacks keep the loop alive.
size_t pendingMainThreadTasks();

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_MAIN_THREAD_DISPATCH_H
