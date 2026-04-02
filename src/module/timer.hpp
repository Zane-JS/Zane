#ifndef Z8_TIMER_H
#define Z8_TIMER_H

#include "v8.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace z8 {
namespace module {

class Timer {
  public:
    static void initialize(v8::Isolate* p_isolate, v8::Local<v8::Context> p_context);

    // Global functions for JS
    static void setTimeout(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void clearTimeout(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setInterval(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void clearInterval(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Event loop integration
    static bool tick(v8::Isolate* p_isolate, v8::Local<v8::Context> p_context);
    static bool hasActiveTimers();
    static std::chrono::milliseconds getNextDelay();

  private:
    struct TimerData {
        int32_t m_id;
        v8::Global<v8::Function> m_callback;
        std::chrono::steady_clock::time_point m_expiry;
        std::vector<v8::Global<v8::Value>> m_args;
        int32_t m_interval_ms;
        bool m_is_interval;
    };

    static std::map<int32_t, std::unique_ptr<TimerData>> m_timers;
    static int32_t m_next_timer_id;
    static int32_t m_running_timer_id;
    static bool m_running_timer_cleared;
};

} // namespace module
} // namespace z8

#endif // Z8_TIMER_H
