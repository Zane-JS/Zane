/* HTTP Loop Manager - Integrates uSockets with Z8 event loop */

#include "http_loop.h"

extern "C" {
#include "libusockets.h"
}

#include <windows.h>

namespace z8 {
namespace module {

HTTPLoopManager::HTTPLoopManager() : p_loop(nullptr), m_active_servers(0) {
    // Create shared uSockets loop
    p_loop = us_create_loop(nullptr, nullptr, nullptr, nullptr, 0);
}

HTTPLoopManager::~HTTPLoopManager() {
    if (p_loop) {
        us_loop_free(p_loop);
    }
}

us_loop_t* HTTPLoopManager::getLoop() {
    return p_loop;
}

void HTTPLoopManager::tick() {
    if (!p_loop || m_active_servers == 0)
        return;

    // Poll IOCP with zero timeout (non-blocking)
    HANDLE iocp_handle = *((HANDLE*)p_loop); // First member is iocp_handle
    
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    LPOVERLAPPED overlapped;
    
    // Non-blocking poll
    BOOL result = GetQueuedCompletionStatus(
        iocp_handle,
        &bytes_transferred,
        &completion_key,
        &overlapped,
        0  // Zero timeout = non-blocking
    );
    
    if (overlapped != NULL) {
        // Process the completion
        // This would normally be handled by us_loop_run(), but we're doing manual polling
        // For now, just acknowledge we got something
    }
}

} // namespace module
} // namespace z8
