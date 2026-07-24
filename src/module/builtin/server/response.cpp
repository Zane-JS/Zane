#include "response.hpp"

namespace zane {
namespace builtin {

v8::Persistent<v8::ObjectTemplate> Response::m_template;

Response::Response(SendCallback send_cb) : m_send_cb(std::move(send_cb)) {}

void Response::setHeader(const std::string& name, const std::string& value) {
    m_headers[name] = value;
}

void Response::send(const std::string& body) {
    if (m_has_ended) return;

    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    if (m_send_cb) {
        m_send_cb(m_status, m_headers, body_bytes);
    }

    m_has_ended = true;
    m_headers_sent = true;
}

void Response::sendJson(v8::Isolate* p_isolate, v8::Local<v8::Value> obj) {
    if (m_has_ended) return;

    v8::HandleScope handle_scope(p_isolate);
    auto context = p_isolate->GetCurrentContext();
    v8::Local<v8::String> json_str = v8::JSON::Stringify(context, obj).ToLocalChecked();
    v8::String::Utf8Value utf8(p_isolate, json_str);

    if (*utf8) {
        setHeader("Content-Type", "application/json");
        send(*utf8);
    }
}

void Response::end() {
    if (m_has_ended) return;
    send("");
}

v8::Local<v8::ObjectTemplate> Response::createTemplate(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    if (!m_template.IsEmpty()) {
        return handle_scope.Escape(m_template.Get(p_isolate));
    }

    v8::Local<v8::ObjectTemplate> tpl = v8::ObjectTemplate::New(p_isolate);
    tpl->SetInternalFieldCount(1);

    // status property (read/write)
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "status"), getStatus, setStatus);

    // headers property (read-only)
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "headers"), getHeaders);

    // Methods
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "send"),
             v8::FunctionTemplate::New(p_isolate, sendMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "json"),
             v8::FunctionTemplate::New(p_isolate, sendJsonMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"),
             v8::FunctionTemplate::New(p_isolate, endMethod));

    m_template.Reset(p_isolate, tpl);
    return handle_scope.Escape(tpl);
}

v8::Local<v8::Object> Response::wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    v8::Local<v8::ObjectTemplate> tpl = createTemplate(p_isolate);
    v8::Local<v8::Object> obj = tpl->NewInstance(context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));

    // Transfer ownership to V8. The weak callback only deletes the object once
    // the response has ended, so a still-pending async response is kept alive
    // even if JS drops its reference to the wrapper before calling send().
    v8::Global<v8::Object> global_obj(p_isolate, obj);
    global_obj.SetWeak(this, weakCallback, v8::WeakCallbackType::kParameter);

    return handle_scope.Escape(obj);
}

void Response::weakCallback(const v8::WeakCallbackInfo<Response>& data) {
    Response* p_res = data.GetParameter();
    // Safety net: an async fetch handler may drop the response object before it
    // has ended. Force-end it (empty body) so the underlying TCP reply is still
    // flushed and the connection is released, then free the memory.
    if (!p_res->m_has_ended) {
        p_res->send("");
    }
    delete p_res;
}

Response* Response::unwrap(v8::Local<v8::Object> obj) {
    v8::Local<v8::Data> field = obj->GetInternalField(0);
    if (field.IsEmpty()) return nullptr;
    v8::Local<v8::Value> val = field.As<v8::Value>();
    if (!val->IsExternal()) return nullptr;
    return static_cast<Response*>(val.As<v8::External>()->Value());
}

// Properties
void Response::getStatus(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Response* p_res = unwrap(info.HolderV2());
    if (!p_res) return;
    info.GetReturnValue().Set(static_cast<int32_t>(p_res->m_status));
}

void Response::setStatus(v8::Local<v8::Name> property, v8::Local<v8::Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
    (void)property;
    Response* p_res = unwrap(info.HolderV2());
    if (!p_res) return;
    p_res->m_status = static_cast<int32_t>(value->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(200));
}

void Response::getHeaders(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Response* p_res = unwrap(info.HolderV2());
    if (!p_res) return;

    v8::Isolate* p_isolate = info.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> headers_obj = v8::Object::New(p_isolate);

    for (const auto& [key, val] : p_res->m_headers) {
        headers_obj
            ->Set(context, v8::String::NewFromUtf8(p_isolate, key.c_str()).ToLocalChecked(),
                  v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked())
            .Check();
    }

    info.GetReturnValue().Set(headers_obj);
}

// Methods
void Response::sendMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended) return;

    v8::String::Utf8Value utf8(args.GetIsolate(), args[0]);
    if (*utf8) {
        p_res->send(*utf8);
    }
}

void Response::sendJsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended || args.Length() < 1) return;

    p_res->sendJson(args.GetIsolate(), args[0]);
}

void Response::endMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended) return;

    if (args.Length() > 0) {
        v8::String::Utf8Value utf8(args.GetIsolate(), args[0]);
        if (*utf8) {
            p_res->send(*utf8);
            return;
        }
    }
    p_res->end();
}

} // namespace builtin
} // namespace zane
