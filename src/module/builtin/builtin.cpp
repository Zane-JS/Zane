#include "builtin.hpp"
#include "server/server.hpp"

namespace zane {
namespace builtin {

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

    // Attach Zane to global
    global->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "Zane"), zane_obj).Check();
}

bool hasActiveWork() {
    return Server::hasActiveServers();
}

} // namespace builtin
} // namespace zane
