#include "timer.hpp"
#include <algorithm>

namespace z8 {
namespace module {

std::map<int32_t, std::unique_ptr<Timer::TimerData>> Timer::m_timers;
int32_t Timer::m_next_timer_id = 1;
int32_t Timer::m_running_timer_id = -1;
bool Timer::m_running_timer_cleared = false;

void Timer::initialize(v8::Isolate* p_isolate, v8::Local<v8::Context> p_context) {
    v8::Local<v8::Object> global = p_context->Global();

    global
        ->Set(p_context,
              v8::String::NewFromUtf8(p_isolate, "setTimeout").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, setTimeout)->GetFunction(p_context).ToLocalChecked())
        .Check();

    global
        ->Set(p_context,
              v8::String::NewFromUtf8(p_isolate, "clearTimeout").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, clearTimeout)->GetFunction(p_context).ToLocalChecked())
        .Check();

    global
        ->Set(p_context,
              v8::String::NewFromUtf8(p_isolate, "setInterval").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, setInterval)->GetFunction(p_context).ToLocalChecked())
        .Check();

    global
        ->Set(p_context,
              v8::String::NewFromUtf8(p_isolate, "clearInterval").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, clearInterval)->GetFunction(p_context).ToLocalChecked())
        .Check();
}

void Timer::setTimeout(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "First argument must be a function").ToLocalChecked());
        return;
    }

    int32_t delay = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        delay = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }
    if (delay < 0)
        delay = 0;

    int32_t id = m_next_timer_id++;
    auto up_timer = std::make_unique<TimerData>();
    up_timer->m_id = id;
    up_timer->m_callback.Reset(p_isolate, args[0].As<v8::Function>());
    up_timer->m_expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    up_timer->m_is_interval = false;
    up_timer->m_interval_ms = 0;

    // Capture extra arguments
    for (int32_t i = 2; i < args.Length(); i++) {
        up_timer->m_args.emplace_back(p_isolate, args[i]);
    }

    m_timers[id] = std::move(up_timer);
    args.GetReturnValue().Set(id);
}

void Timer::setInterval(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "First argument must be a function").ToLocalChecked());
        return;
    }

    int32_t delay = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        delay = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }
    if (delay < 1)
        delay = 1; // Minimum interval is 1ms to prevent infinite synchronous loops

    int32_t id = m_next_timer_id++;
    auto up_timer = std::make_unique<TimerData>();
    up_timer->m_id = id;
    up_timer->m_callback.Reset(p_isolate, args[0].As<v8::Function>());
    up_timer->m_expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    up_timer->m_is_interval = true;
    up_timer->m_interval_ms = delay;

    // Capture extra arguments
    for (int32_t i = 2; i < args.Length(); i++) {
        up_timer->m_args.emplace_back(p_isolate, args[i]);
    }

    m_timers[id] = std::move(up_timer);
    args.GetReturnValue().Set(id);
}

void Timer::clearInterval(const v8::FunctionCallbackInfo<v8::Value>& args) {
    clearTimeout(args);
}

void Timer::clearTimeout(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsNumber())
        return;
    int32_t id = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(-1);

    if (id == m_running_timer_id) {
        m_running_timer_cleared = true;
    }

    m_timers.erase(id);
}

bool Timer::tick(v8::Isolate* p_isolate, v8::Local<v8::Context> p_context) {
    if (m_timers.empty())
        return false;

    auto now = std::chrono::steady_clock::now();
    std::vector<int32_t> to_run;

    for (auto const& [id, up_timer] : m_timers) {
        if (up_timer->m_expiry <= now) {
            to_run.push_back(id);
        }
    }

    // Sort by expiry to maintain order for same-time timers
    std::sort(
        to_run.begin(), to_run.end(), [](int32_t a, int32_t b) { return m_timers[a]->m_expiry < m_timers[b]->m_expiry; });

    for (int32_t id : to_run) {
        auto it = m_timers.find(id);
        if (it == m_timers.end())
            continue;

        auto up_timer = std::move(it->second);
        m_timers.erase(it);

        m_running_timer_id = id;
        m_running_timer_cleared = false;

        v8::Local<v8::Function> cb = up_timer->m_callback.Get(p_isolate);

        std::vector<v8::Local<v8::Value>> js_args;
        for (auto& arg : up_timer->m_args) {
            js_args.push_back(arg.Get(p_isolate));
        }

        // Call the callback. If it throws, the isolation outer TryCatch in main.cpp will see it.
        v8::MaybeLocal<v8::Value> result =
            cb->Call(p_context, p_context->Global(), static_cast<int32_t>(js_args.size()), js_args.data());

        m_running_timer_id = -1;

        if (result.IsEmpty()) {
            return false; // Stop the event loop as something went wrong
        }

        // Reschedule if it's an interval and wasn't cleared during execution
        if (up_timer->m_is_interval && !m_running_timer_cleared) {
            up_timer->m_expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(up_timer->m_interval_ms);
            m_timers[up_timer->m_id] = std::move(up_timer);
        }
    }

    return !m_timers.empty();
}

bool Timer::hasActiveTimers() {
    return !m_timers.empty();
}

std::chrono::milliseconds Timer::getNextDelay() {
    if (m_timers.empty())
        return std::chrono::milliseconds(0);

    auto now = std::chrono::steady_clock::now();
    auto min_expiry = std::chrono::steady_clock::time_point::max();

    for (auto const& [id, up_timer] : m_timers) {
        if (up_timer->m_expiry < min_expiry) {
            min_expiry = up_timer->m_expiry;
        }
    }

    if (min_expiry <= now)
        return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(min_expiry - now);
}

} // namespace module
} // namespace z8
