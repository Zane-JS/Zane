#ifndef Z8_TASK_QUEUE_H
#define Z8_TASK_QUEUE_H

#include "v8.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

namespace z8 {

struct Task {
    v8::Global<v8::Function> m_callback;
    v8::Global<v8::Promise::Resolver> m_resolver;
    bool m_is_promise;
    std::function<void(v8::Isolate*, v8::Local<v8::Context>, Task*)> m_runner;

    // Data (managed by the specific task)
    void* p_data;
    int32_t m_error_code;
};

class TaskQueue {
  public:
    static TaskQueue& getInstance() {
        static TaskQueue s_instance;
        return s_instance;
    }

    void enqueue(Task* p_task) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push(p_task);
        }
        m_condition.notify_one(); // Wake up Main Thread
    }

    Task* dequeue() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty())
            return nullptr;
        Task* p_task = m_queue.front();
        m_queue.pop();
        return p_task;
    }

    bool isEmpty() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    // New: Blocking wait for the main thread
    void wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_queue.empty())
            return;
        m_condition.wait_for(lock, timeout);
    }

  private:
    std::queue<Task*> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condition;
};

} // namespace z8

#endif
