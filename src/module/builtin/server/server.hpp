#ifndef ZANE_BUILTIN_SERVER_H
#define ZANE_BUILTIN_SERVER_H

#include "v8.h"
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace zane {
namespace builtin {

class Request;
class Response;

// Callback type: handle HTTP request. The handler runs on a Drogon event-loop
// thread; the V8 isolate is locked/unlocked inside the handler.
//
// NOTE: `p_req` and `p_res` are raw, non-owning pointers. Ownership is
// transferred to the V8 wrapper inside the handler (via Request::wrap /
// Response::wrap), which frees them through a V8 weak callback. The handler
// must NOT delete them. The Drogon response callback (`cb`) is invoked exactly
// once from `Response::finalize()` — it may be called from the V8 thread after
// a pending Promise settles; Drogon dispatches the actual socket write on the
// connection's event-loop thread (queueInLoop), so this is thread-safe.
using RequestHandler = std::function<void(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)>;

class Server {
  public:
    Server();
    ~Server();

    // Start listening (registers a listener + default handler on the shared
    // Drogon app singleton, then runs the Drogon event loop on a background
    // thread). `stop()` must be called to tear it down.
    bool start(
        uint16_t port, 
        const std::string& hostname, 
        RequestHandler handler,
        bool use_tls,
        const std::string& cert_path,
        const std::string& key_path);

    // Stop server (quits the Drogon app and joins the loop thread).
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

  private:
    // Drogon's event loop runs on this thread; it returns when quit() is
    // called. Stopped/joined in stop().
    std::thread m_loop_thread;
    std::function<void()> m_on_stop;
    bool m_running = false;

    static std::atomic<int32_t> m_active_server_count;
};

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_SERVER_H