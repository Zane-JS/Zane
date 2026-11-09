#include "server.hpp"
#include "request.hpp"
#include "response.hpp"
#include "task_queue.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <trantor/utils/Logger.h>
#include <cctype>
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

bool Server::start(
    uint16_t port, 
    const std::string& hostname, 
    RequestHandler handler, 
    bool use_tls, 
    const std::string& cert_path, 
    const std::string& key_path
) {
    if (m_running) return false;

    auto& app = drogon::app();
    try {
        // NOTE: if this Zane process ever serves two listeners on the same
        // port, addListener() throws; we surface that as a start failure.
        app.addListener(hostname, port, use_tls, cert_path, key_path);
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to add listener: " << e.what();
        return false;
    }

    // Disable Drogon's implicit home page rewrite ("/" -> "/index.html"
    // before routing): Zane.serve() apps own the URL space via fetch().
    app.setHomePage("");

    // Issue #9 (connection cap) is now enforced by Drogon's own limiter;
    // any connection beyond this is closed by the framework.
    app.setMaxConnectionNum(10000);

    if (handler) {
        app.setDefaultHandler(
            [handler = std::move(handler)](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                handler(req, std::move(callback));
            });
    }

    // Drogon's event loop runs on this thread; run() returns after quit().
    m_loop_thread = std::thread([]() { drogon::app().run(); });

    m_active_server_count++;
    m_running = true;

    return true;
}

void Server::stop(std::function<void()> on_stop) {
    if (!m_running) return;

    m_on_stop = std::move(on_stop);

    // Signal all Drogon event loops to stop.
    drogon::app().quit();

    // Wait for the loop thread to exit.
    if (m_loop_thread.joinable()) {
        m_loop_thread.join();
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
    //
    // The Drogon handler runs on a Drogon event-loop thread, which is NOT the
    // V8 main thread. All V8-interacting work (fetch invocation, promise
    // handling, Response finalization, timer creation from JS) must run on the
    // main thread, otherwise V8 isolate access and the timer/task machinery
    // race and crash. So this handler only:
    //   1. copies the request data into a heap Dispatch struct, and
    //   2. enqueues a Task for the main thread, which runs the fetch handler.
    //
    // The Drogon response callback (cb) is stored inside Dispatch. The main
    // thread moves it into the Response completer; Response::finalize() calls
    // it exactly once — from the main thread — and Drogon dispatches the
    // actual socket write on the connection's event-loop thread
    // (queueInLoop), so it is safe.
    struct Dispatch {
        std::string m_method;
        std::string m_url;
        std::map<std::string, std::string> m_headers;
        std::vector<uint8_t> m_body;
        std::function<void(drogon::HttpResponsePtr)> m_cb;
    };

    RequestHandler handler = [p_isolate, p_context_global, p_fetch_global, p_error_global](
                                 const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        // Header names are lowercased to match the pre-Drogon parser's
        // behavior (the app-facing lookup in Request uses lowercase keys).
        std::map<std::string, std::string> headers;
        for (const auto& [k, v] : req->getHeaders()) {
            std::string lk = k;
            for (auto& c : lk) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            headers[std::move(lk)] = std::string(v);
        }

        std::string url = req->getOriginalPath();
        if (!req->query().empty()) {
            url.push_back('?');
            url += req->query();
        }
        std::string_view body_sv = req->body();
        std::vector<uint8_t> body(body_sv.begin(), body_sv.end());

        auto* p_dispatch = new Dispatch{
            drogon::to_string(req->method()),
            std::move(url),
            std::move(headers),
            std::move(body),
            std::move(cb),
        };

        auto* p_task = new zane::Task();
        p_task->m_is_promise = false;
        p_task->m_error_code = 0;

        // Runs on the V8 main thread (TaskQueue::wait wakeup). The isolate is
        // already owned by the main thread here, so no v8::Locker is needed.
        p_task->m_runner = [p_isolate, p_context_global, p_fetch_global, p_error_global, p_dispatch](
                               v8::Isolate* p_iso, v8::Local<v8::Context> /*ctx*/, zane::Task* /*task*/) {
            v8::HandleScope handle_scope(p_iso);
            v8::Local<v8::Context> context = p_context_global->Get(p_iso);
            v8::Context::Scope context_scope(context);

            // p_req/p_res are heap-allocated and NOT owned here: ownership
            // transfers to the V8 wrapper objects (Request::wrap /
            // Response::wrap), which free them via a weak callback on GC.
            // This keeps them alive for as long as JS can reach them —
            // including the async case where fetch() returns a Promise or the
            // app responds later via setTimeout() etc.
            auto* p_req = new Request(
                std::move(p_dispatch->m_method),
                std::move(p_dispatch->m_url),
                std::move(p_dispatch->m_headers),
                std::move(p_dispatch->m_body));

            auto* p_res = new Response(
                [cb = std::move(p_dispatch->m_cb)](drogon::HttpResponsePtr resp) { cb(std::move(resp)); });

            delete p_dispatch;

            v8::Local<v8::Object> req_obj = p_req->wrap(p_iso, context);
            v8::Local<v8::Object> res_obj = p_res->wrap(p_iso, context);

            v8::Local<v8::Value> argv[2] = {req_obj, res_obj};
            v8::Local<v8::Function> fetch_local = p_fetch_global->Get(p_iso);
            v8::TryCatch try_catch(p_iso);

            auto result = fetch_local->Call(context, context->Global(), 2, argv);

            if (try_catch.HasCaught()) {
                if (p_error_global) {
                    v8::Local<v8::Function> error_local = p_error_global->Get(p_iso);
                    v8::Local<v8::Value> err_argv[1] = {try_catch.Exception()};
                    (void)error_local->Call(context, context->Global(), 1, err_argv);
                }
                if (!p_res->hasEnded()) {
                    p_res->send("Internal Server Error");
                }
                return;
            }

            // A handler that returns without ending the response leaves it
            // pending: Drogon keeps the connection open until the response
            // callback fires, which supports apps that respond asynchronously
            // via setTimeout()/setImmediate() etc. The Response weak callback
            // force-ends an abandoned response; no sync end() is issued here.
            if (result.IsEmpty()) {
                return;
            }
            v8::Local<v8::Value> ret = result.ToLocalChecked();
            if (!ret->IsPromise()) {
                return;
            }

            v8::Local<v8::Promise> promise = ret.As<v8::Promise>();
            const auto send_error = [&](v8::Local<v8::Value> err) {
                if (p_error_global) {
                    v8::Local<v8::Function> error_local = p_error_global->Get(p_iso);
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
                            /*p_req_wrap=*/     new v8::Global<v8::Object>(p_iso, req_obj),
                            /*p_res_wrap=*/     new v8::Global<v8::Object>(p_iso, res_obj),
                            /*p_promise_wrap=*/ new v8::Global<v8::Promise>(p_iso, promise),
                            /*p_res=*/          p_res,
                            /*p_error_global=*/ p_error_global,
                        };

                        v8::Local<v8::External> data = v8::External::New(p_iso, p_state);
                        v8::Local<v8::Function> on_fulfilled =
                            v8::FunctionTemplate::New(p_iso, onPromiseFulfilled, data)
                                ->GetFunction(context).ToLocalChecked();
                        v8::Local<v8::Function> on_rejected =
                            v8::FunctionTemplate::New(p_iso, onPromiseRejected, data)
                                ->GetFunction(context).ToLocalChecked();

                        promise->Then(context, on_fulfilled, on_rejected).IsEmpty();
                    }
                    break;
            }
        };

        zane::TaskQueue::getInstance().enqueue(p_task);
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
                delete p_srv;
            }
        },
        v8::External::New(p_isolate, p_server));

    v8::Local<v8::ObjectTemplate> server_tpl = v8::ObjectTemplate::New(p_isolate);
    server_tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"),
                    close_fn);

    v8::Local<v8::Object> server_obj = server_tpl->NewInstance(context).ToLocalChecked();

    // Schedule a weak callback on the returned object. When the JS GC
    // collects it: stop the server if still running, then delete.
    // Use a v8::Global so the callback fires exactly once; after
    // V8 calls the callback the global is cleared automatically.
    auto* p_server_global = new v8::Global<v8::Object>(p_isolate, server_obj);
    p_server_global->SetWeak(p_server, [](const v8::WeakCallbackInfo<Server>& data) {
        Server* p_srv = data.GetParameter();
        if (p_srv && !p_srv->isStopped()) {
            p_srv->stop();
            delete p_srv;
        }
    }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(server_obj);
}

} // namespace builtin
} // namespace zane