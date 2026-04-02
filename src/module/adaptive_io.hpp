#ifndef Z8_ADAPTIVE_IO_H
#define Z8_ADAPTIVE_IO_H

#include <chrono>
#include <cstdio>
#include <mutex>
#include <functional>

namespace z8 {
namespace module {

/**
 * AdaptiveIO provides a mechanism to balance between low-latency and high-throughput.
 * It automatically detects high-frequency I/O bursts and switches from immediate
 * flushing to buffered I/O to maximize performance.
 */
class AdaptiveIO {
public:
    AdaptiveIO(int32_t burst_threshold = 20, int32_t window_ms = 50)
        : m_burst_threshold(burst_threshold), m_window_ms(window_ms),
          m_calls_in_burst(0), m_last_flush(std::chrono::steady_clock::now()) {}

    /**
     * Decisions whether the I/O should be flushed based on current burst frequency.
     * @param flush A callback function that performs the actual flush/syscall.
     */
    template<typename FlushFunc>
    void apply(FlushFunc&& flush) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_flush).count();

        if (elapsed < m_window_ms) {
            m_calls_in_burst++;
        } else {
            m_calls_in_burst = 0;
            m_last_flush = now;
        }

        // Logic: 
        // 1. If we are in a burst (>= threshold), we DON'T flush. 
        //    This allows the underlying buffers (libc or internal) to consolidate.
        // 2. If we are NOT in a burst, we flush immediately to ensure interactivity.
        if (m_calls_in_burst < m_burst_threshold) {
            flush();
        }
    }

    /**
     * Specialization for standard FILE* streams.
     */
    void flushIfNeeded(FILE* p_stream) {
        if (!p_stream) return;
        apply([p_stream]() {
            if (p_stream == stderr) {
                std::fflush(stdout);
            }
            std::fflush(p_stream);
        });
    }

    /**
     * Configures a stream to use Z8's optimized 64KB full buffering.
     */
    static void setupBuffer(FILE* p_stream, size_t size = 64 * 1024) {
        if (p_stream) {
            std::setvbuf(p_stream, nullptr, _IOFBF, static_cast<int>(size));
        }
    }

private:
    int32_t m_burst_threshold;
    int32_t m_window_ms;
    int32_t m_calls_in_burst;
    std::chrono::steady_clock::time_point m_last_flush;
    std::mutex m_mutex;
};

// Global shared instances for standard streams to ensure consistent 
// adaptive behavior across modules (console, fs, etc.)
inline AdaptiveIO g_stdout_io;
inline AdaptiveIO g_stderr_io;

} // namespace module
} // namespace z8

#endif // Z8_ADAPTIVE_IO_H
