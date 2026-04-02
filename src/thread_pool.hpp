#ifndef Z8_THREAD_POOL_H
#define Z8_THREAD_POOL_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace z8 {

class ThreadPool {
  public:
    static ThreadPool& getInstance() {
        static ThreadPool s_instance(static_cast<size_t>(std::thread::hardware_concurrency()));
        return s_instance;
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto sp_task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = sp_task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            if (m_stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            m_tasks.emplace([sp_task]() { (*sp_task)(); });
        }
        m_condition.notify_one();
        return res;
    }

    bool hasPendingTasks() {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        return !m_tasks.empty() || m_active_tasks > 0;
    }

  private:
    ThreadPool(size_t threads) : m_stop(false), m_active_tasks(0) {
        for (size_t i = 0; i < threads; ++i)
            m_workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->m_queue_mutex);
                        this->m_condition.wait(lock, [this] { return this->m_stop || !this->m_tasks.empty(); });
                        if (this->m_stop && this->m_tasks.empty())
                            return;
                        task = std::move(this->m_tasks.front());
                        this->m_tasks.pop();
                        this->m_active_tasks++;
                    }
                    task();
                    {
                        std::unique_lock<std::mutex> lock(this->m_queue_mutex);
                        this->m_active_tasks--;
                    }
                }
            });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_stop = true;
        }
        m_condition.notify_all();
        for (std::thread& worker : m_workers)
            worker.join();
    }

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_queue_mutex;
    std::condition_variable m_condition;
    bool m_stop;
    int32_t m_active_tasks;
};

} // namespace z8

#endif
