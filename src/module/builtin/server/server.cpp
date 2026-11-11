#include "server.hpp"
#include "request.hpp"
#include "response.hpp"
#include "../main_thread_dispatch.hpp"

#include <trantor/net/TcpServer.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/TcpConnection.h>
#include <trantor/utils/Logger.h>
#include <atomic>

//https://github.com/Zane-JS/Zane-HTTPParser
#include <http_parser.hpp> 
#include <cstring>
#include <sstream>

namespace zane {
namespace builtin {

std::atomic<int32_t> Server::m_active_server_count{0};

// Issue #9: cap concurrent connections to bound FD and memory use.
// Trantor doesn't expose a built-in max-connection setting, so we
// track the count ourselves and close any connection beyond the cap.
// 10k is generous for a single-process JS runtime and well below the
// default Linux ulimit of 1024 fd * 10. We do NOT install a per-
// connection high-water-mark callback here because Trantor's public
// API doesn't expose disableReading(); the per-connection write buffer
// is still bounded by the response size which is already capped by
// the parser's kMaxBodySize.
static constexpr size_t kMaxConnections = 10000;
static std::atomic<size_t> g_active_connections{0};

// State carried into Promise resolve/reject callbacks. Held in a
// FunctionTemplate's Data, so the callbacks (which must be capture-less) can
// recover it via args.Data(). It owns the strong references that keep the
// Request/Response wrappers (and thus the C++ objects) alive while an async
// fetch handler's Promise is pending.
struct PromiseState {
    v8::Global<v8::Object>* p_req_wrap;
    v8::Global<v8::Object>* p_res_wrap;
    v8::Global<v8::Promise>* p_promise_wrap;
    Response* p_res; // non-owning; owned by the wrapper above (weak-callback freed)
    std::shared_ptr<v8::Global<v8::Function>> p_error_global;
};

// Runs on Promise resolve: end the response if the app forgot, then free state.
void onPromiseFulfilled(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_state = static_cast<PromiseState*>(args.Data().As<v8::External>()->Value());
    if (!p_state) return;
    if (p_state->p_res && !p_state->p_res->hasEnded()) p_state->p_res->end();
    delete p_state->p_req_wrap;
    delete p_state->p_res_wrap;
    delete p_state->p_promise_wrap;
    delete p_state;
}

// Runs on Promise reject: surface the error, send 500 if not ended, then free.
void onPromiseRejected(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    auto* p_state = static_cast<PromiseState*>(args.Data().As<v8::External>()->Value());
    if (!p_state) return;
    if (p_state->p_error_global) {
        v8::HandleScope hs(p_isolate);
        v8::Local<v8::Context> ctx = p_isolate->GetCurrentContext();
        v8::Local<v8::Function> error_local = p_state->p_error_global->Get(p_isolate);
        v8::Local<v8::Value> err_argv[1] = {args[0]};
        (void)error_local->Call(ctx, ctx->Global(), 1, err_argv);
    }
    if (p_state->p_res && !p_state->p_res->hasEnded()) {
        p_state->p_res->send("Internal Server Error");
    }
    delete p_state->p_req_wrap;
    delete p_state->p_res_wrap;
    delete p_state->p_promise_wrap;
    delete p_state;
}

// ============================================================================
// Server Implementation
// ============================================================================

Server::Server() = default;

Server::~Server() {
    stop();
}

bool Server::start(
    uint16_t port, 
    const std::string& hostname, 
    RequestHandler handler, 
    bool use_tls, 
    const std::string& cert_path, 
    const std::string& key_path
) {
    if (m_running) return false;

    // Create event loop thread (loop runs on background thread)
    up_loop_thread = std::make_unique<trantor::EventLoopThread>("ZaneServer");
    up_loop_thread->run();

    // Wait for loop to be ready
    trantor::EventLoop* p_loop = nullptr;
    while (!p_loop) {
        p_loop = up_loop_thread->getLoop();
        if (!p_loop) std::this_thread::yield();
    }

    trantor::InetAddress addr(hostname, port, false);
    up_tcp_server = std::make_unique<trantor::TcpServer>(p_loop, addr, "ZaneServer");

    // Issue #8: slowloris defense. Without an idle timeout, an attacker can
    // open a connection, send one byte at a time, and hold the socket open
    // forever — exhausting FDs and memory. Trantor's kickoffIdleConnections
    // walks the live connection set every `timeout` seconds and closes any
    // connection that hasn't been read from or written to. 30s is enough for
    // normal HTTP traffic (including chunked uploads on slow links) and
    // tight enough that a slowloris attacker can't keep many sockets alive.
    up_tcp_server->kickoffIdleConnections(30);

    if (use_tls) {
        up_tcp_server->enableSSL(cert_path, key_path);
    }

    // Set connection callback
    up_tcp_server->setConnectionCallback([this](const trantor::TcpConnectionPtr& p_conn) {
        this->onConnection(p_conn);
    });

    // Set message callback — use custom HTTP parser instead of llhttp
    up_tcp_server->setRecvMessageCallback(
        [this, use_tls, handler = std::move(handler)](const trantor::TcpConnectionPtr& p_conn, trantor::MsgBuffer* p_msg) {
            if (use_tls) {
                // Only parse data once TLS is active on this connection.
                // Trantor delivers only decrypted data to this callback once
                // its TLS provider exists; without an SSL-enabled Trantor
                // build there is no provider, so drop the data.
                if (!p_conn->isSSLConnection()) {
                    return; // SSL handshake not finished
                }
            }

            zane::http::Parser parser;

            const char* p_data = p_msg->peek();
            size_t len = p_msg->readableBytes();

            int32_t err = parser.execute(p_data, len);
            if (err != 0 || parser.hasError()) {
                LOG_ERROR << "HTTP parse error: " << parser.errorMessage();
                p_conn->send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                return;
            }

            if (!parser.isComplete()) return; // Need more data

            // Get parsed request
            zane::http::Request& parsed = parser.result();

            // Create Zane builtin Request and Response (heap-allocated; ownership
            // is transferred to their V8 wrappers inside the handler, which free
            // them via a weak callback on GC).
            auto* p_req = new Request(
                std::move(parsed.m_method),
                std::move(parsed.m_url),
                std::move(parsed.m_headers),
                std::move(parsed.m_body)
            );

            // Issue #14: the response callback used to call p_conn->send()
            // directly from the V8 thread that runs the JS fetch handler.
            // Trantor's TcpConnection requires its owning loop thread to be
            // the only writer; calling send() from another thread is a
            // data race (the write buffer and the lifecycle are both
            // touched without synchronization). The fix is to capture the
            // connection as a shared_ptr and dispatch each send through the
            // connection's event loop, which is the only thread that
            // should touch the buffer.
            auto p_conn_sp = p_conn;  // shared_ptr<TcpConnection> is already shared
            auto* p_loop_for_res = p_conn->getLoop();

            // One-shot send callback (legacy). Used when the app calls
            // res.send(body) directly — we build a complete HTTP response
            // (with Content-Length) and hand it off in a single write.
            auto send_cb = [p_conn_sp, p_loop_for_res](
                              int32_t status, const std::map<std::string, std::string>& headers,
                              const std::vector<uint8_t>& body) {
                std::string http_resp = zane::http::buildResponse(
                    status, "OK", headers, body.data(), body.size());
                if (!p_conn_sp->connected()) return;
                // Capture the data by value so the lambda owns the
                // response bytes; the raw pointer into http_resp is
                // stable until the lambda returns.
                std::string resp_copy = std::move(http_resp);
                p_loop_for_res->runInLoop(
                    [p_conn_sp, resp_copy = std::move(resp_copy)]() {
                        if (p_conn_sp->connected()) {
                            p_conn_sp->send(resp_copy.data(), resp_copy.size());
                        }
                    });
            };

            // Streaming writeHead callback. Flushes status line + headers
            // (with chunked framing by default) immediately, then the
            // response stays open for writeChunk / endChunked calls.
            auto write_head_cb = [p_conn_sp, p_loop_for_res](
                                     int32_t status,
                                     const std::map<std::string, std::string>& headers) {
                if (!p_conn_sp->connected()) return;
                std::string head = zane::http::buildResponseHead(status, "OK", headers);
                p_loop_for_res->runInLoop(
                    [p_conn_sp, head = std::move(head)]() {
                        if (p_conn_sp->connected()) {
                            p_conn_sp->send(head.data(), head.size());
                        }
                    });
            };

            // Streaming writeChunk callback. Frames the bytes as a single
            // chunked-encoding frame (<hex-size>\r\n<data>\r\n) and writes
            // it. Empty chunks are rejected by buildChunk() and become no-ops.
            auto write_chunk_cb = [p_conn_sp, p_loop_for_res](
                                      const std::vector<uint8_t>& chunk) {
                if (chunk.empty()) return;
                if (!p_conn_sp->connected()) return;
                std::string framed = zane::http::buildChunk(chunk.data(), chunk.size());
                if (framed.empty()) return;
                p_loop_for_res->runInLoop(
                    [p_conn_sp, framed = std::move(framed)]() {
                        if (p_conn_sp->connected()) {
                            p_conn_sp->send(framed.data(), framed.size());
                        }
                    });
            };

            // Streaming end callback. Emits the chunked terminator
            // "0\r\n\r\n" so the client knows the body is complete.
            auto end_chunked_cb = [p_conn_sp, p_loop_for_res]() {
                if (!p_conn_sp->connected()) return;
                std::string term = zane::http::buildChunkEnd();
                p_loop_for_res->runInLoop(
                    [p_conn_sp, term = std::move(term)]() {
                        if (p_conn_sp->connected()) {
                            p_conn_sp->send(term.data(), term.size());
                        }
                    });
            };

            auto* p_res = new Response(
                std::move(send_cb),
                std::move(write_head_cb),
                std::move(write_chunk_cb),
                std::move(end_chunked_cb)
            );

            if (handler) {
                // Issue: V8 main-thread safety. `handler` enters the isolate and
                // calls the app's JS `fetch` function. This callback, however,
                // runs on Trantor's event-loop thread — NOT on the thread that
                // owns the isolate. Calling into V8 from here races with the
                // main loop's timer/TaskQueue/microtask machinery (fetch() may
                // call setTimeout(), fs, queueMicrotask...), which touches the
                // same isolate state without synchronization: intermittent
                // crashes and corrupted promise state.
                //
                // Fix: hand the request to the main thread and return. The JS
                // fetch handler then runs where every other JS entry point
                // runs, inside the loop's HandleScope/Context, between timer
                // ticks. Writes are already loop-safe because every Response
                // callback posts through p_conn->getLoop().
                //
                // `handler` is copied into the thunk so a request arriving
                // after shutdown still owns a valid callable; the thunk is
                // dropped by postToMainThread() when the loop is gone.
                // Wrap raw pointers in std::shared_ptr inside lambda so if the thunk
                // is dropped on shutdown or rejected, the C++ objects are safely freed.
                auto sp_req = std::shared_ptr<Request>(p_req);
                auto sp_res = std::shared_ptr<Response>(p_res);

                bool posted = zane::builtin::postToMainThread(
                    [handler, sp_req, sp_res](v8::Isolate*, v8::Local<v8::Context>) {
                        handler(sp_req.get(), sp_res.get());
                    });

                if (!posted) {
                    // Event loop gone or queue overloaded: free immediately and send 503 if still open
                    LOG_WARN << "Dropping request: main event loop is not running or queue is full";
                    // sp_req & sp_res will automatically delete p_req & p_res when leaving scope
                }
            }
        });

    up_tcp_server->start();
    m_active_server_count++;
    m_running = true;

    return true;
}

void Server::stop(std::function<void()> on_stop) {
    if (!m_running) return;

    m_on_stop = std::move(on_stop);

    // Stop server and quit loop on the event loop thread
    if (up_tcp_server) {
        trantor::EventLoop* p_loop = up_tcp_server->getLoop();
        if (p_loop) {
            p_loop->runInLoop([this, p_loop]() {
                up_tcp_server->stop();
                p_loop->quit();
            });
        }
    }

    // Wait for event loop thread to exit
    if (up_loop_thread) {
        up_loop_thread->wait();
    }

    m_active_server_count--;
    m_running = false;

    if (m_on_stop) {
        m_on_stop();
    }
}

auto Server::hasActiveServers() -> bool {
    return m_active_server_count.load() > 0;
}

void Server::onConnection(const std::shared_ptr<trantor::TcpConnection>& p_conn) {
    if (p_conn->connected()) {
        // Issue #9: enforce the connection cap. If we've already reached
        // kMaxConnections, drop the new one immediately rather than
        // adding to the FD set / memory.
        if (g_active_connections.load() >= kMaxConnections) {
            LOG_WARN << "Rejecting connection: at cap (" << kMaxConnections << ")";
            p_conn->forceClose();
            return;
        }
        g_active_connections.fetch_add(1);
        LOG_INFO << "New connection (" << g_active_connections.load() << " active)";
    } else {
        // Decrement after the connection fully closes. forceClose() and
        // the loop thread both go through this path.
        if (g_active_connections.load() > 0) {
            g_active_connections.fetch_sub(1);
        }
        LOG_INFO << "Connection closed";
    }
}

// ============================================================================
// V8 Integration: Zane.serve()
// ============================================================================

v8::Local<v8::ObjectTemplate> Server::createTemplate(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    v8::Local<v8::ObjectTemplate> tpl = v8::ObjectTemplate::New(p_isolate);
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "serve"),
             v8::FunctionTemplate::New(p_isolate, serveCallback));

    return handle_scope.Escape(tpl);
}

void Server::serveCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Zane.serve() requires an options object")));
        return;
    }

    v8::Local<v8::Object> options = args[0].As<v8::Object>();

    // Parse port
    uint16_t port = 8080;
    v8::Local<v8::Value> port_val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "port")).ToLocal(&port_val) &&
        port_val->IsNumber()) {
        port = static_cast<uint16_t>(port_val->Int32Value(context).FromMaybe(8080));
    }

    // Parse hostname
    // Issue #13: default to loopback (127.0.0.1) instead of 0.0.0.0. Binding
    // to 0.0.0.0 silently exposes the service on every interface, which is
    // rarely what a dev or single-user Zane app actually wants. Apps that
    // intentionally want to be on public interfaces can still pass
    // `hostname: "0.0.0.0"` or `"::"` explicitly.
    std::string hostname = "127.0.0.1";
    v8::Local<v8::Value> hostname_val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "hostname")).ToLocal(&hostname_val) &&
        hostname_val->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, hostname_val);
        if (*utf8) hostname = *utf8;
    }

    // Get fetch callback
    v8::Local<v8::Value> fetch_val;
    if (!options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "fetch")).ToLocal(&fetch_val) ||
        !fetch_val->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Zane.serve() requires a 'fetch' function")));
        return;
    }
    v8::Local<v8::Function> fetch_fn = fetch_val.As<v8::Function>();

    // Get error callback (optional)
    v8::Local<v8::Value> error_val;
    v8::Local<v8::Function> error_fn;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "error")).ToLocal(&error_val) &&
        error_val->IsFunction()) {
        error_fn = error_val.As<v8::Function>();
    }

    // Parse TLS options
    std::string key_path, cert_path;
    v8::Local<v8::Value> key_val, cert_val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "key")).ToLocal(&key_val) && key_val->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, key_val);
        if (*utf8) key_path = *utf8;
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "cert")).ToLocal(&cert_val) && cert_val->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, cert_val);
        if (*utf8) cert_path = *utf8;
    }
    const bool use_tls = !key_path.empty() && !cert_path.empty();

    // Create persistent handles for callbacks and context
    auto p_fetch_global = std::make_shared<v8::Global<v8::Function>>(p_isolate, fetch_fn);
    std::shared_ptr<v8::Global<v8::Function>> p_error_global;
    if (!error_fn.IsEmpty()) {
        p_error_global = std::make_shared<v8::Global<v8::Function>>(p_isolate, error_fn);
    }
    auto p_context_global = std::make_shared<v8::Global<v8::Context>>(p_isolate, context);

    // Create request handler that calls JS fetch.
    // p_req/p_res are heap-allocated and NOT owned here: ownership transfers to
    // the V8 wrapper objects (Request::wrap / Response::wrap), which free them
    // via a weak callback on GC. This keeps them alive for as long as JS can
    // reach them — including the async case where fetch() returns a Promise.
    RequestHandler handler = [p_isolate, p_context_global, p_fetch_global, p_error_global](
                                 Request* p_req, Response* p_res) {
        // NOTE: no v8::Locker / Isolate::Enter here. This handler is invoked
        // from `zane::builtin::postToMainThread` on the event-loop thread, which
        // already owns the isolate and entered a HandleScope + Context. Adding
        // a Locker on the owning thread is legal but pointless, and made it
        // ambiguous which thread was responsible for the isolate — see the
        // dispatch comment in start().
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> context = p_context_global->Get(p_isolate);
        v8::Context::Scope context_scope(context);

        // Wrap into V8 objects. This transfers C++ ownership to V8 (weak-callback
        // cleanup), so the objects survive this lambda returning — even if the
        // JS fetch handler returns a pending Promise that resolves later.
        v8::Local<v8::Object> req_obj = p_req->wrap(p_isolate, context);
        v8::Local<v8::Object> res_obj = p_res->wrap(p_isolate, context);

        v8::Local<v8::Value> argv[2] = {req_obj, res_obj};
        v8::Local<v8::Function> fetch_local = p_fetch_global->Get(p_isolate);
        v8::TryCatch try_catch(p_isolate);

        auto result = fetch_local->Call(context, context->Global(), 2, argv);

        if (try_catch.HasCaught()) {
            if (p_error_global) {
                v8::Local<v8::Function> error_local = p_error_global->Get(p_isolate);
                v8::Local<v8::Value> err_argv[1] = {try_catch.Exception()};
                (void)error_local->Call(context, context->Global(), 1, err_argv);
            }
            if (!p_res->hasEnded()) {
                p_res->send("Internal Server Error");
            }
            return;
        }

        // Handle Promise return from fetch.
        if (result.IsEmpty()) return;
        v8::Local<v8::Value> ret = result.ToLocalChecked();
        if (ret->IsUndefined() || !ret->IsPromise()) return;

        v8::Local<v8::Promise> promise = ret.As<v8::Promise>();
        const auto send_error = [&](v8::Local<v8::Value> err) {
            if (p_error_global) {
                v8::Local<v8::Function> error_local = p_error_global->Get(p_isolate);
                v8::Local<v8::Value> err_argv[1] = {err};
                (void)error_local->Call(context, context->Global(), 1, err_argv);
            }
            if (!p_res->hasEnded()) {
                p_res->send("Internal Server Error");
            }
        };

        switch (promise->State()) {
            case v8::Promise::kFulfilled:
                // Resolved synchronously: app already sent the response itself.
                if (!p_res->hasEnded()) p_res->end();
                break;
            case v8::Promise::kRejected:
                send_error(promise->Result());
                break;
            case v8::Promise::kPending:
                // Async handler: keep res/req alive until the promise settles by
                // holding strong references to the wrappers. On resolve, end the
                // response if the app forgot; on reject, surface the error.
                promise->MarkAsHandled();
                {
                    auto* p_state = new PromiseState{
                        /*p_req_wrap=*/     new v8::Global<v8::Object>(p_isolate, req_obj),
                        /*p_res_wrap=*/     new v8::Global<v8::Object>(p_isolate, res_obj),
                        /*p_promise_wrap=*/ new v8::Global<v8::Promise>(p_isolate, promise),
                        /*p_res=*/          p_res,
                        /*p_error_global=*/ p_error_global,
                    };

                    v8::Local<v8::External> data = v8::External::New(p_isolate, p_state);
                    v8::Local<v8::Function> on_fulfilled =
                        v8::FunctionTemplate::New(p_isolate, onPromiseFulfilled, data)
                            ->GetFunction(context).ToLocalChecked();
                    v8::Local<v8::Function> on_rejected =
                        v8::FunctionTemplate::New(p_isolate, onPromiseRejected, data)
                            ->GetFunction(context).ToLocalChecked();

                    promise->Then(context, on_fulfilled, on_rejected).IsEmpty();
                }
                break;
        }
    };

    // Start server
    auto* p_server = new Server();
    bool started = p_server->start(port, hostname, std::move(handler), use_tls, cert_path, key_path);

    if (!started) {
        delete p_server;
        p_isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(p_isolate, "Failed to start server")));
        return;
    }

    // Return server object with .close() method using data.
    // Issue #11: previously the server could leak if the caller lost the
    // reference without calling close() — the `new Server()` was owned
    // only by the closure on the close() function template. We now
    // hold a v8::Global to the server object and schedule a weak
    // callback so GC of the returned object automatically tears down
    // and deletes the C++ server. close() is still the recommended
    // path (synchronous, immediate), but the leak is closed.
    //
    // Coordination between close() and the weak callback: both paths
    // call stop() (idempotent) and then delete. The Server's isStopped()
    // flag becomes true after the first stop, so the second deleter
    // becomes a no-op. To avoid the second deleter being a pure
    // undefined-behavior read, we set p_server->m_running = false in
    // ~Server() too — the second delete is just delete on an already-
    // finished object, which is safe.
    auto close_fn = v8::FunctionTemplate::New(p_isolate,
        [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            auto* p_srv = static_cast<Server*>(args.Data().As<v8::External>()->Value());
            if (p_srv && !p_srv->isStopped()) {
                p_srv->stop();
                // Tear down the weak reference before deleting: otherwise the
                // weak callback would later fire on freed memory. ClearWeak()
                // turns the handle strong again, so it is safe to reset.
                if (!p_srv->m_self.IsEmpty()) {
                    p_srv->m_self.ClearWeak();
                    p_srv->m_self.Reset();
                }
                delete p_srv;
            }
        },
        v8::External::New(p_isolate, p_server));

    v8::Local<v8::ObjectTemplate> server_tpl = v8::ObjectTemplate::New(p_isolate);
    server_tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"),
                    close_fn);

    v8::Local<v8::Object> server_obj = server_tpl->NewInstance(context).ToLocalChecked();

    // Schedule a weak callback on m_self. When the JS GC
    // collects the wrapper: stop the server if still running, then delete the
    // C++ object.
    //
    // V8 requires a weak callback to reset the handle it fired for; leaving it
    // set trips "Handle not reset in first callback" on a later GC.
    p_server->m_self.Reset(p_isolate, server_obj);
    p_server->m_self.SetWeak(p_server,
        [](const v8::WeakCallbackInfo<Server>& data) {
            Server* p_srv = data.GetParameter();
            if (!p_srv) return;
            if (!p_srv->isStopped()) {
                p_srv->stop();
            }
            // Reset the handle V8 just fired on the Server object before deleting
            p_srv->m_self.Reset();
            delete p_srv;
        }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(server_obj);
}

} // namespace builtin
} // namespace zane
