#include "response.hpp"
#include <http_parser.hpp>

namespace zane {
namespace builtin {

v8::Persistent<v8::ObjectTemplate> Response::m_template;

bool containsHeaderInjection(const std::string& s) {
    // Reject any CR / LF / NUL: these terminate header lines and let an
    // attacker inject arbitrary headers (response splitting) or smuggle a
    // second response. NUL also breaks C-string handling downstream.
    return s.find('\r') != std::string::npos ||
           s.find('\n') != std::string::npos ||
           s.find('\0') != std::string::npos;
}

// ============================================================================
// Construction
// ============================================================================

Response::Response(SendCallback send_cb) : m_send_cb(std::move(send_cb)) {}

Response::Response(SendCallback send_cb,
                   WriteHeadCallback write_head_cb,
                   WriteChunkCallback write_chunk_cb,
                   EndChunkedCallback end_chunked_cb)
    : m_send_cb(std::move(send_cb)),
      m_write_head_cb(std::move(write_head_cb)),
      m_write_chunk_cb(std::move(write_chunk_cb)),
      m_end_chunked_cb(std::move(end_chunked_cb)) {}

// ============================================================================
// Header / status helpers
// ============================================================================

void Response::setHeader(const std::string& name, const std::string& value) {
    // Defense-in-depth: the serializer (buildResponse) also strips CRLF, but
    // reject early at the application-facing API so the caller gets a clear
    // error instead of a silently mangled response.
    if (name.empty() || containsHeaderInjection(name) || containsHeaderInjection(value)) {
        return; // JS-facing setHeader() throws instead; internal callers ignore.
    }
    m_headers[name] = value;
}

// ============================================================================
// Internal: flush the response head when the first body byte is about to go
// out, so the application can mix legacy one-shot send() with new streaming
// write() / writeHead() without having to know which mode is in use.
// ============================================================================

void Response::flushHeadIfNeeded() {
    if (m_head_sent) return;
    if (m_has_ended) return;
    if (!m_write_head_cb) {
        // Streaming callbacks weren't wired up — fall back to the legacy
        // one-shot path by sending everything inline. This keeps any
        // external embedder that constructs a Response with just the
        // SendCallback (e.g. tests) source-compatible.
        return;
    }
    // Streaming mode forces Transfer-Encoding: chunked framing. Apps that
    // want Content-Length should NOT use write()/writeHead() and instead
    // use send() with a known-size body.
    m_write_head_cb(m_status, m_headers);
    m_head_sent = true;
}

// ============================================================================
// One-shot API (legacy)
// ============================================================================

void Response::send(const std::string& body) {
    if (m_has_ended) return;

    // Streaming path: the response has been put into streaming mode by a
    // prior writeHead() or write() call, so we cannot use Content-Length
    // (we don't know the total size yet). Flush head if needed, write the
    // body as one chunk, and terminate.
    if (m_streaming && m_write_chunk_cb && m_end_chunked_cb) {
        flushHeadIfNeeded();
        if (!body.empty()) {
            std::vector<uint8_t> chunk(body.begin(), body.end());
            m_write_chunk_cb(chunk);
        }
        m_end_chunked_cb();
        m_has_ended = true;
        return;
    }

    // Legacy non-streaming path (Content-Length framed). This is the
    // original behavior pre-streaming and is kept identical for backward
    // compatibility — every existing test that uses res.send() must
    // continue to produce a Content-Length response.
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    if (m_send_cb) {
        m_send_cb(m_status, m_headers, body_bytes);
    }

    m_has_ended = true;
    m_head_sent = true;
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

    // Streaming path: flush head if needed and emit the chunked terminator.
    if (m_streaming && m_write_chunk_cb && m_end_chunked_cb) {
        flushHeadIfNeeded();
        m_end_chunked_cb();
        m_has_ended = true;
        return;
    }

    // Legacy path: send an empty body so Content-Length: 0 is emitted.
    send("");
}

// ============================================================================
// Streaming API
// ============================================================================

void Response::writeHead(int32_t status, const std::map<std::string, std::string>& headers) {
    if (m_has_ended) return;
    if (m_head_sent) {
        // Head already flushed. The JS-facing writeHeadMethod() wrapper
        // detects this state and throws a JS error; from pure C++ callers
        // we silently no-op (matches the legacy send() / end() pattern).
        return;
    }

    // Caller-supplied headers REPLACE the headers collected via setHeader()
    // so far, matching Node.js. If the caller passed an empty map, keep the
    // existing collected headers — that lets apps do the common pattern:
    //   res.setHeader("X-Foo", "1");
    //   res.writeHead(200, { "Content-Type": "text/plain" });
    // without having to re-declare X-Foo.
    if (!headers.empty()) {
        m_headers = headers;
    }
    m_status = status;
    m_write_head_called = true;
    m_streaming = true;

    if (m_write_head_cb) {
        m_write_head_cb(m_status, m_headers);
        m_head_sent = true;
    }
}

void Response::write(const std::vector<uint8_t>& chunk) {
    if (m_has_ended) return;
    if (chunk.empty()) return; // empty mid-stream is a no-op; terminator goes through end().

    if (!m_write_chunk_cb) {
        // Streaming wasn't wired up; fall back to one-shot semantics.
        std::string body(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        send(body);
        return;
    }

    m_streaming = true;
    flushHeadIfNeeded();
    m_write_chunk_cb(chunk);
}

void Response::write(const std::string& chunk) {
    if (m_has_ended) return;
    if (chunk.empty()) return;
    std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
    write(bytes);
}

// ============================================================================
// V8 wiring
// ============================================================================

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
             v8::FunctionTemplate::New(p_isolate, jsonMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "status"),
             v8::FunctionTemplate::New(p_isolate, statusMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"),
             v8::FunctionTemplate::New(p_isolate, endMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "setHeader"),
             v8::FunctionTemplate::New(p_isolate, setHeaderMethod));
    // Streaming methods
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeHead"),
             v8::FunctionTemplate::New(p_isolate, writeHeadMethod));
    tpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"),
             v8::FunctionTemplate::New(p_isolate, writeMethod));

    m_template.Reset(p_isolate, tpl);
    return handle_scope.Escape(tpl);
}

v8::Local<v8::Object> Response::wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    v8::Local<v8::ObjectTemplate> tpl = createTemplate(p_isolate);
    v8::Local<v8::Object> obj = tpl->NewInstance(context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));

    // Transfer ownership to V8: the weak callback frees the C++ object once the
    // JS wrapper is collected.
    //
    // The handle is stored on the object (m_self) and made weak in place. V8
    // requires the weak callback to reset this exact handle; keeping it in a
    // local that dies at the end of wrap() causes
    // "Handle not reset in first callback".
    m_self.Reset(p_isolate, obj);
    m_self.SetWeak(this, weakCallback, v8::WeakCallbackType::kParameter);

    return handle_scope.Escape(obj);
}

void Response::weakCallback(const v8::WeakCallbackInfo<Response>& data) {
    Response* p_res = data.GetParameter();
    if (!p_res) return;
    // V8 forbids calling back into the V8 API from a first-pass weak callback,
    // and requires the handle to be reset. Reset first, then do pure C++ work.
    p_res->m_self.Reset();
    // A still-pending response must still produce a reply, otherwise the client
    // hangs and the connection is never released. send("") only touches the
    // connection's event loop (a plain std::function), never V8.
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

// ============================================================================
// JS-facing property accessors
// ============================================================================

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

// ============================================================================
// JS-facing methods
// ============================================================================

void Response::sendMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended) return;

    v8::String::Utf8Value utf8(args.GetIsolate(), args[0]);
    if (*utf8) {
        p_res->send(*utf8);
    }
}

void Response::jsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended || args.Length() < 1) return;

    p_res->sendJson(args.GetIsolate(), args[0]);
}

void Response::statusMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || args.Length() < 1 || !args[0]->IsNumber()) {
        args.GetReturnValue().Set(args.This());
        return;
    }
    
    p_res->setStatus(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(200));
    args.GetReturnValue().Set(args.This());
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

void Response::writeHeadMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res) return;

    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    // Signature: writeHead(status, [headers]). Both arguments are optional;
    // if omitted, status defaults to the existing m_status and headers stay
    // as previously collected via setHeader(). This mirrors Node.js.
    int32_t status = p_res->m_status;
    std::map<std::string, std::string> headers;

    if (args.Length() >= 1 && args[0]->IsNumber()) {
        status = static_cast<int32_t>(args[0]->Int32Value(context).FromMaybe(200));
    } else if (args.Length() >= 1 && !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate,
                "res.writeHead() requires (status, [headers])")));
        return;
    }

    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> hobj = args[1].As<v8::Object>();
        v8::Local<v8::Array> props;
        if (hobj->GetPropertyNames(context).ToLocal(&props)) {
            uint32_t len = props->Length();
            for (uint32_t i = 0; i < len; i++) {
                v8::Local<v8::Value> key_val = props->Get(context, i).ToLocalChecked();
                v8::Local<v8::Value> val_val = hobj->Get(context, key_val).ToLocalChecked();

                v8::String::Utf8Value k(p_isolate, key_val);
                v8::String::Utf8Value v(p_isolate, val_val);
                if (!*k || !*v) continue;

                std::string name(*k, k.length());
                std::string val(*v, v.length());

                if (name.empty() || containsHeaderInjection(name) || containsHeaderInjection(val)) {
                    p_isolate->ThrowException(v8::Exception::RangeError(
                        v8::String::NewFromUtf8Literal(p_isolate,
                            "Invalid header: name/value must not be empty or contain CR/LF/NUL")));
                    return;
                }
                headers[name] = val;
            }
        }
    } else if (args.Length() >= 2 && !args[1]->IsUndefined()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate,
                "res.writeHead() headers must be an object")));
        return;
    }

    p_res->writeHead(status, headers);
}

void Response::writeMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Response* p_res = unwrap(args.This());
    if (!p_res || p_res->m_has_ended || args.Length() < 1) return;

    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope hs(p_isolate);
    v8::Local<v8::Value> arg = args[0];

    // Accept either a string or a Uint8Array. Strings are coerced to UTF-8
    // bytes (matches Node.js http module). Uint8Array / Buffer pass through
    // as-is so binary streaming (file downloads, SSE, etc.) works.
    if (arg->IsUint8Array()) {
        v8::Local<v8::Uint8Array> u8 = arg.As<v8::Uint8Array>();
        size_t len = u8->Length();
        if (len == 0) return;
        std::vector<uint8_t> chunk(len);
        u8->CopyContents(chunk.data(), len);
        p_res->write(chunk);
        return;
    }

    v8::String::Utf8Value utf8(p_isolate, arg);
    if (*utf8) {
        p_res->write(std::string(*utf8, utf8.length()));
    }
}

void Response::setHeaderMethod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    Response* p_res = unwrap(args.This());
    if (!p_res) return;

    // Mirror node:http: name and value are coerced to strings.
    if (args.Length() < 2) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate,
                "res.setHeader(name, value) requires 2 arguments")));
        return;
    }

    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::String::Utf8Value name_utf8(p_isolate, args[0]);
    v8::String::Utf8Value value_utf8(p_isolate, args[1]);
    if (!*name_utf8 || !*value_utf8) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate,
                "res.setHeader() name and value must be strings")));
        return;
    }

    std::string name(*name_utf8, name_utf8.length());
    std::string value(*value_utf8, value_utf8.length());

    // Reject anything that could split the header / smuggle a response.
    if (name.empty() || containsHeaderInjection(name) || containsHeaderInjection(value)) {
        p_isolate->ThrowException(v8::Exception::RangeError(
            v8::String::NewFromUtf8Literal(p_isolate,
                "Invalid header: name/value must not be empty or contain CR/LF/NUL")));
        return;
    }

    p_res->m_headers[name] = value;
}

} // namespace builtin
} // namespace zane
