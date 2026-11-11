#ifndef ZANE_BUILTIN_SERVER_REQUEST_H
#define ZANE_BUILTIN_SERVER_REQUEST_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace zane {
namespace builtin {

class Request {
  public:
    Request(std::string method, std::string path, std::map<std::string, std::string> headers,
            std::vector<uint8_t> body);

    // JS accessors
    auto method() const -> const std::string& { return m_method; }
    auto url() const -> const std::string& { return m_path; }
    auto pathname() const -> const std::string& { return m_pathname; }
    auto headers() const -> const std::map<std::string, std::string>& { return m_headers; }

    // Native storage retained while a request waits for main-thread dispatch.
    size_t retainedBytes() const {
        size_t bytes = sizeof(Request) + m_method.capacity() + m_path.capacity() +
                       m_pathname.capacity() + m_body.capacity();
        for (const auto& header : m_headers) {
            bytes += sizeof(header) + 4 * sizeof(void*) +
                     header.first.capacity() + header.second.capacity();
        }
        return bytes;
    }

    // Body helpers (C++ → V8)
    v8::Local<v8::Value> json(v8::Isolate* p_isolate, v8::Local<v8::Context> context);
    v8::Local<v8::String> text(v8::Isolate* p_isolate);

    // V8 object wrapper. Ownership of `this` is transferred to the JS object:
    // it is freed via the weak callback when V8 GCs the wrapper. Callers must
    // NOT delete the returned object.
    v8::Local<v8::Object> wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    // Template factory
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

  private:
    // V8 weak callback — frees the C++ Request when the JS wrapper is GC'd.
    static void weakCallback(const v8::WeakCallbackInfo<Request>& data);

    // Body parsing
    void parseBody(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    std::string m_method;
    std::string m_path;
    std::string m_pathname; // Parsed path (before ?)
    std::map<std::string, std::string> m_headers;
    std::vector<uint8_t> m_body;
    v8::Global<v8::Value> m_parsed_body; // Cached parsed body (JSON, form-data)

    // Internal V8 template (cached)
    static v8::Persistent<v8::ObjectTemplate> m_template;

    // Persistent handle to the JS wrapper. Must live on the object: V8 requires
    // a weak callback to reset the handle it fired for.
    v8::Global<v8::Object> m_self;

    // JS property getters
    static void getMethod(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getUrl(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getPathname(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getHeaders(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getBody(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

    // JS methods
    static void jsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void textMethod(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Internal: extract Request* from V8 object
    static Request* unwrap(v8::Local<v8::Object> obj);
};

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_SERVER_REQUEST_H
