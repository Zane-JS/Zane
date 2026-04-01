/* HTTP Manager - Integrates uSockets IOCP loop with Z8 event loop */

#include "http_manager.h"

extern "C" {
#include "libusockets.h"
}

#include <windows.h>

namespace z8 {
namespace module {

HTTPManager::HTTPManager() : p_loop(nullptr) {
    // Create shared uSockets loop for all HTTP servers
    p_loop = us_create_loop(nullptr, nullptr, nullptr, nullptr, 0);
}

HTTPManager::~HTTPManager() {
    if (p_loop) {
        us_loop_free(p_loop);
    }
}

us_loop_t* HTTPManager::getLoop() {
    return p_loop;
}

void HTTPManager::registerServer(HTTPServer* server) {
    m_servers.push_back(server);
}

void HTTPManager::unregisterServer(HTTPServer* server) {
    m_servers.erase(
        std::remove(m_servers.begin(), m_servers.end(), server),
        m_servers.end()
    );
}

bool HTTPManager::hasActiveServers() const {
    return !m_servers.empty();
}

void HTTPManager::tick() {
    if (!p_loop || m_servers.empty())
        return;

    // Poll IOCP with zero timeout (non-blocking)
    // Access the IOCP handle from the loop structure
    struct us_loop_internal {
        void* data;
        HANDLE iocp_handle;
        // ... other fields
    };
    
    us_loop_internal* loop_internal = (us_loop_internal*)p_loop;
    HANDLE iocp_handle = loop_internal->iocp_handle;
    
    if (!iocp_handle)
        return;
    
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    LPOVERLAPPED overlapped;
    
    // Non-blocking poll (0ms timeout)
    BOOL result = GetQueuedCompletionStatus(
        iocp_handle,
        &bytes_transferred,
        &completion_key,
        &overlapped,
        0  // Zero timeout = non-blocking
    );
    
    if (overlapped != NULL || result) {
        // We got a completion event
        // The actual processing is done by uSockets internal functions
        // which are triggered by the IOCP completion
        
        // For now, we just acknowledge that we polled
        // The real implementation would need to call us_internal_dispatch_ready_poll
        // but that requires more integration work
    }
}

} // namespace module
} // namespace z8
