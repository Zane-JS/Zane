#ifndef ZANE_BUILTIN_SERVER_H
#define ZANE_BUILTIN_SERVER_H

#include "v8.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Forward declarations
namespace trantor {
class TcpServer;
class TcpConnection;
class EventLoopThread;
} // namespace trantor

namespace zane {
namespace builtin {

class Request;
class Response;

// Callback type: handle HTTP request.
// NOTE: p_req and p_res are raw, non-owning pointers. Ownership is transferred
// to the V8 wrapper inside the handler (via Request::wrap / Response::wrap),
// which frees them through a V8 weak callback. The handler must NOT delete them.
using RequestHandler = std::function<void(Request* p_req, Response* p_res)>;

class Server {
  public:
    Server();
    ~Server();

    // Start listening
    bool start(
        uint16_t port, 
        const std::string& hostname, 
        RequestHandler handler,
        bool use_tls,
        const std::string& cert_path,
        const std::string& key_path);

    // Stop server
    void stop(std::function<void()> on_stop = nullptr);

    // Check if server has active connections
    static auto hasActiveServers() -> bool;

    // V8 factory: Zane.serve({...})
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    // V8 static callback: Zane.serve() (used by builtin registration)
    static void serveCallback(const v8::FunctionCallbackInfo<v8::Value>& args);

    // True if `stop()` has been called (or the weak callback has run).
    // The weak callback and the explicit close() both need to coordinate
    // so we don't double-delete the Server.
    bool isStopped() const { return !m_running; }

    // Handle to the JS wrapper returned by Zane.serve(). The weak callback must
    // reset it, and m_self_owner keeps the Global object itself alive until the
    // callback runs (freeing a Global before its callback would reset an
    // already-freed slot).
    v8::Global<v8::Object> m_self;
    v8::Global<v8::Object>* m_self_owner = nullptr;

  private:
    std::unique_ptr<trantor::TcpServer> up_tcp_server;
    std::unique_ptr<trantor::EventLoopThread> up_loop_thread;
    std::function<void()> m_on_stop;
    bool m_running = false;

    static std::atomic<int32_t> m_active_server_count;

    // Internal Trantor callbacks
    void onConnection(const std::shared_ptr<trantor::TcpConnection>& p_conn);
};

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_SERVER_H
