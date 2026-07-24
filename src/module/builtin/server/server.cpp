#include "server.hpp"
#include "request.hpp"
#include "response.hpp"

#include <trantor/net/TcpServer.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/TcpConnection.h>
#include <trantor/utils/Logger.h>

//https://github.com/Zane-JS/Zane-HTTPParser
#include <http_parser.hpp> 
#include <cstring>
#include <sstream>

namespace zane {
namespace builtin {

std::atomic<int32_t> Server::m_active_server_count{0};

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

bool Server::start(uint16_t port, const std::string& hostname, RequestHandler handler) {
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

    // Set connection callback
    up_tcp_server->setConnectionCallback([this](const trantor::TcpConnectionPtr& p_conn) {
        this->onConnection(p_conn);
    });

    // Set message callback — use custom HTTP parser instead of llhttp
    up_tcp_server->setRecvMessageCallback(
        [this, handler = std::move(handler)](const trantor::TcpConnectionPtr& p_conn, trantor::MsgBuffer* p_msg) {
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

            auto* p_res = new Response(
                [p_conn](int32_t status, const std::map<std::string, std::string>& headers,
                          const std::vector<uint8_t>& body) {
                    std::string http_resp = zane::http::buildResponse(
                        status, "OK", headers, body.data(), body.size());
                    p_conn->send(http_resp.data(), http_resp.size());
                }
            );

            if (handler) {
                handler(p_req, p_res);
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
        LOG_INFO << "New connection";
    } else {
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
    std::string hostname = "0.0.0.0";
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
        v8::Locker locker(p_isolate);
        v8::Isolate::Scope isolate_scope(p_isolate);
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
    bool started = p_server->start(port, hostname, std::move(handler));

    if (!started) {
        delete p_server;
        p_isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(p_isolate, "Failed to start server")));
        return;
    }

    // Return server object with .close() method using data
    auto close_fn = v8::FunctionTemplate::New(p_isolate,
        [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            auto* p_srv = static_cast<Server*>(args.Data().As<v8::External>()->Value());
            p_srv->stop();
            delete p_srv;
        },
        v8::External::New(p_isolate, p_server));

    v8::Local<v8::ObjectTemplate> server_tpl = v8::ObjectTemplate::New(p_isolate);
    server_tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"),
                    close_fn);

    v8::Local<v8::Object> server_obj = server_tpl->NewInstance(context).ToLocalChecked();
    args.GetReturnValue().Set(server_obj);
}

} // namespace builtin
} // namespace zane
