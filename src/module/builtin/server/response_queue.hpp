#pragma once

#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace zane::builtin {

// All operations run on the connection's event loop. A streaming response
// keeps its slot until its terminator is written.
class ResponseQueue {
  public:
    struct PendingResponse {
        std::string m_bytes;
        bool m_complete = false;
    };

    std::shared_ptr<PendingResponse> enqueue() {
        auto sp_pending = std::make_shared<PendingResponse>();
        m_pending.push_back(sp_pending);
        return sp_pending;
    }

    template <typename Writer>
    void write(const std::shared_ptr<PendingResponse>& sp_pending,
               std::string bytes, bool complete, Writer writer) {
        sp_pending->m_bytes += bytes;
        sp_pending->m_complete = complete;
        while (!m_pending.empty()) {
            auto sp_front = m_pending.front();
            if (!sp_front->m_bytes.empty()) {
                writer(sp_front->m_bytes);
                sp_front->m_bytes.clear();
            }
            if (!sp_front->m_complete) break;
            m_pending.pop_front();
        }
    }

  private:
    std::deque<std::shared_ptr<PendingResponse>> m_pending;
};

} // namespace zane::builtin
