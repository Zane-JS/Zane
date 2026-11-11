#include "request.hpp"

namespace zane {
namespace builtin {

v8::Persistent<v8::ObjectTemplate> Request::m_template;

#include "urldecode.h"

void Request::parseBody(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    if (!m_parsed_body.IsEmpty()) return; // Already parsed

    auto it = m_headers.find("content-type");
    if (it == m_headers.end() || m_body.empty()) {
        m_parsed_body.Reset(p_isolate, v8::Null(p_isolate));
        return;
    }

    std::string content_type = it->second;
    v8::Local<v8::Value> parsed;

    if (content_type.find("application/json") != std::string::npos) {
        parsed = json(p_isolate, context);
    } else if (content_type.find("application/x-www-form-urlencoded") != std::string::npos) {
        std::string body_str(reinterpret_cast<const char*>(m_body.data()), m_body.size());
        v8::Local<v8::Object> obj = v8::Object::New(p_isolate);

        size_t start = 0;
        while (start < body_str.length()) {
            size_t end = body_str.find('&', start);
            if (end == std::string::npos) {
                end = body_str.length();
            }
            std::string pair_str = body_str.substr(start, end - start);
            size_t eq_pos = pair_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = url_decode(pair_str.substr(0, eq_pos));
                std::string val = url_decode(pair_str.substr(eq_pos + 1));
                obj->Set(context, v8::String::NewFromUtf8(p_isolate, key.c_str()).ToLocalChecked(), v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked()).Check();
            }
            start = end + 1;
        }
        parsed = obj;
    } else {
        parsed = v8::Null(p_isolate);
    }

    m_parsed_body.Reset(p_isolate, parsed);
}

Request::Request(std::string method, std::string path, std::map<std::string, std::string> headers,
                 std::vector<uint8_t> body)
    : m_method(std::move(method)), m_path(std::move(path)), m_headers(std::move(headers)), m_body(std::move(body)) {
    auto qpos = m_path.find('?');
    m_pathname = (qpos != std::string::npos) ? m_path.substr(0, qpos) : m_path;
}

v8::Local<v8::Value> Request::json(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    std::string body_str(reinterpret_cast<const char*>(m_body.data()), m_body.size());
    v8::Local<v8::String> v8_str = v8::String::NewFromUtf8(p_isolate, body_str.c_str()).ToLocalChecked();
    v8::Local<v8::Value> undefined = v8::Undefined(p_isolate);
    v8::Local<v8::Value> result = v8::JSON::Parse(context, v8_str).FromMaybe(undefined);

    return handle_scope.Escape(result);
}

v8::Local<v8::String> Request::text(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    std::string body_str(reinterpret_cast<const char*>(m_body.data()), m_body.size());
    v8::Local<v8::String> result = v8::String::NewFromUtf8(p_isolate, body_str.c_str()).ToLocalChecked();

    return handle_scope.Escape(result);
}

v8::Local<v8::ObjectTemplate> Request::createTemplate(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    if (!m_template.IsEmpty()) {
        return handle_scope.Escape(m_template.Get(p_isolate));
    }

    v8::Local<v8::ObjectTemplate> tpl = v8::ObjectTemplate::New(p_isolate);
    tpl->SetInternalFieldCount(1);

    // Read-only properties via SetNativeDataProperty
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "method"), getMethod);
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "url"), getUrl);
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "pathname"), getPathname);
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "headers"), getHeaders);
    tpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "body"), getBody);

    // Methods
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "json"),
             v8::FunctionTemplate::New(p_isolate, jsonMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "text"),
             v8::FunctionTemplate::New(p_isolate, textMethod));

    m_template.Reset(p_isolate, tpl);
    return handle_scope.Escape(tpl);
}

v8::Local<v8::Object> Request::wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    v8::Local<v8::ObjectTemplate> tpl = createTemplate(p_isolate);
    v8::Local<v8::Object> obj = tpl->NewInstance(context).ToLocalChecked();

    obj->SetInternalField(0, v8::External::New(p_isolate, this));

    // Transfer ownership to V8: free the C++ Request when the JS wrapper is GC'd.
    // This keeps `this` alive as long as the JS object is reachable, which is
    // required because async fetch handlers may hold/use the request long after
    // the recv callback returns.
    //
    // The handle lives on the object and is made weak in place, so the callback
    // can reset the very handle it fired for (see Response::wrap).
    m_self.Reset(p_isolate, obj);
    m_self.SetWeak(this, weakCallback, v8::WeakCallbackType::kParameter);

    return handle_scope.Escape(obj);
}

void Request::weakCallback(const v8::WeakCallbackInfo<Request>& data) {
    Request* p_req = data.GetParameter();
    if (!p_req) return;
    // V8 requires the callback to reset the handle it fired for.
    p_req->m_self.Reset();
    delete p_req;
}

Request* Request::unwrap(v8::Local<v8::Object> obj) {
    v8::Local<v8::Data> field = obj->GetInternalField(0);
    if (field.IsEmpty()) return nullptr;
    v8::Local<v8::Value> val = field.As<v8::Value>();
    if (!val->IsExternal()) return nullptr;
    return static_cast<Request*>(val.As<v8::External>()->Value());
}

// Properties
void Request::getMethod(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Request* p_req = unwrap(info.HolderV2());
    if (!p_req) return;
    info.GetReturnValue().Set(
        v8::String::NewFromUtf8(info.GetIsolate(), p_req->m_method.c_str()).ToLocalChecked());
}

void Request::getUrl(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Request* p_req = unwrap(info.HolderV2());
    if (!p_req) return;
    info.GetReturnValue().Set(
        v8::String::NewFromUtf8(info.GetIsolate(), p_req->m_path.c_str()).ToLocalChecked());
}

void Request::getPathname(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Request* p_req = unwrap(info.HolderV2());
    if (!p_req) return;
    info.GetReturnValue().Set(
        v8::String::NewFromUtf8(info.GetIsolate(), p_req->m_pathname.c_str()).ToLocalChecked());
}

void Request::getHeaders(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Request* p_req = unwrap(info.HolderV2());
    if (!p_req) return;

    v8::Isolate* p_isolate = info.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> headers_obj = v8::Object::New(p_isolate);

    for (const auto& [key, val] : p_req->m_headers) {
        headers_obj
            ->Set(context, v8::String::NewFromUtf8(p_isolate, key.c_str()).ToLocalChecked(),
                  v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked())
            .Check();
    }

    info.GetReturnValue().Set(headers_obj);
}

void Request::getBody(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    (void)property;
    Request* p_req = unwrap(info.HolderV2());
    if (!p_req) return;

    v8::Isolate* p_isolate = info.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    p_req->parseBody(p_isolate, context);
    
    if (p_req->m_parsed_body.IsEmpty()) {
        info.GetReturnValue().Set(v8::Null(p_isolate));
    } else {
        info.GetReturnValue().Set(p_req->m_parsed_body.Get(p_isolate));
    }
}

// Methods
void Request::jsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Request* p_req = unwrap(args.This());
    if (!p_req) return;
    args.GetReturnValue().Set(p_req->json(args.GetIsolate(), args.GetIsolate()->GetCurrentContext()));
}

void Request::textMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Request* p_req = unwrap(args.This());
    if (!p_req) return;
    args.GetReturnValue().Set(p_req->text(args.GetIsolate()));
}

} // namespace builtin
} // namespace zane
