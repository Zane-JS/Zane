#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace zane::builtin {

// All operations run on the connection's event loop.
class ResponseQueue {
  public:
    static constexpr size_t kMaxPendingResponses = 128;
    static constexpr size_t kMaxBufferedBytes = 8 * 1024 * 1024;

    struct PendingResponse {
        std::string m_bytes;
        bool m_complete = false;
        bool m_close = false;
    };

    std::shared_ptr<PendingResponse> enqueue() {
        if (m_closing || m_pending.size() >= kMaxPendingResponses) return nullptr;
        auto sp_pending = std::make_shared<PendingResponse>();
        m_pending.push_back(sp_pending);
        return sp_pending;
    }

    bool closing() const { return m_closing; }

    void closeAfter(const std::shared_ptr<PendingResponse>& sp_pending) {
        sp_pending->m_close = true;
        m_closing = true;
    }

    void abort() {
        m_closing = true;
        m_aborted = true;
        for (const auto& sp_pending : m_pending) {
            std::string().swap(sp_pending->m_bytes);
        }
        m_pending.clear();
        m_buffered_bytes = 0;
    }

    // Returns false on overflow. The caller must close the connection.
    // The front response writes directly, so its size and streaming behavior
    // are unchanged. Only responses waiting behind it count toward the cap.
    template <typename Writer>
    bool write(const std::shared_ptr<PendingResponse>& sp_pending,
               std::string bytes, bool complete, Writer writer) {
        if (m_aborted) return true;
        if (m_pending.front() != sp_pending) {
            if (bytes.size() > kMaxBufferedBytes - m_buffered_bytes) {
                abort();
                return false;
            }
            m_buffered_bytes += bytes.size();
            sp_pending->m_bytes += bytes;
            sp_pending->m_complete = complete;
            return true;
        }

        writer(bytes, complete && sp_pending->m_close);
        if (!complete) return true;
        bool close = sp_pending->m_close;
        m_pending.pop_front();
        if (close) {
            abort();
            return true;
        }
        while (!m_pending.empty()) {
            auto sp_front = m_pending.front();
            m_buffered_bytes -= sp_front->m_bytes.size();
            writer(sp_front->m_bytes, sp_front->m_complete && sp_front->m_close);
            std::string().swap(sp_front->m_bytes);
            if (!sp_front->m_complete) break;
            close = sp_front->m_close;
            m_pending.pop_front();
            if (close) {
                abort();
                break;
            }
        }
        return true;
    }

  private:
    std::deque<std::shared_ptr<PendingResponse>> m_pending;
    size_t m_buffered_bytes = 0;
    bool m_closing = false;
    bool m_aborted = false;
};

} // namespace zane::builtin
