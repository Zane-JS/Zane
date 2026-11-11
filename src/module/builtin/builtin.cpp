#include "builtin.hpp"
#include "main_thread_dispatch.hpp"
#include "server/server.hpp"
#include "../adaptive_io.hpp"

namespace zane {
namespace builtin {

static void setAdaptiveIOCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() > 0 && args[0]->IsBoolean()) {
        bool enabled = args[0]->BooleanValue(p_isolate);
        zane::module::AdaptiveIO::setEnabled(enabled);
    }
}

static void isAdaptiveIOEnabledCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    bool enabled = zane::module::AdaptiveIO::isEnabled();
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, enabled));
}

void registerBuiltins(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Object> global) {
    v8::HandleScope handle_scope(p_isolate);
    v8::Context::Scope context_scope(context);

    // Create Zane global namespace
    v8::Local<v8::Object> zane_obj = v8::Object::New(p_isolate);

    // Register Zane.serve()
    v8::Local<v8::FunctionTemplate> serve_fn_tpl =
        v8::FunctionTemplate::New(p_isolate, Server::serveCallback);
    zane_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "serve"),
              serve_fn_tpl->GetFunction(context).ToLocalChecked())
        .Check();

    // Register Zane.setAdaptiveIO()
    v8::Local<v8::FunctionTemplate> set_adaptive_io_tpl =
        v8::FunctionTemplate::New(p_isolate, setAdaptiveIOCallback);
    zane_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "setAdaptiveIO"),
              set_adaptive_io_tpl->GetFunction(context).ToLocalChecked())
        .Check();

    // Register Zane.isAdaptiveIOEnabled()
    v8::Local<v8::FunctionTemplate> is_adaptive_io_tpl =
        v8::FunctionTemplate::New(p_isolate, isAdaptiveIOEnabledCallback);
    zane_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "isAdaptiveIOEnabled"),
              is_adaptive_io_tpl->GetFunction(context).ToLocalChecked())
        .Check();

    // Attach Zane to global
    global->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "Zane"), zane_obj).Check();
}

bool hasActiveWork() {
    // A live listener keeps the process alive, and so does any callback posted
    // from a background thread (an accepted connection waiting for its JS
    // fetch handler, a pending write completion) — otherwise the loop could
    // decide there is nothing left to do and exit with requests in flight.
    return Server::hasActiveServers() || pendingMainThreadTasks() > 0;
}

} // namespace builtin
} // namespace zane
