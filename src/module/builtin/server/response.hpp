#ifndef ZANE_BUILTIN_SERVER_RESPONSE_H
#define ZANE_BUILTIN_SERVER_RESPONSE_H

#include "v8.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace zane {
namespace builtin {

// Completer invoked exactly once when the response is finalized (send/end).
// The callback receives a fully-built Drogon HttpResponse; Drogon dispatches
// the write on the connection's event-loop thread, so calling it from the V8
// thread (e.g. after an async Promise settles) is safe.
using ResponseCompleter = std::function<void(drogon::HttpResponsePtr)>;

// True if `s` contains a character that could terminate / split an HTTP header
// line (CR, LF) or otherwise corrupt the wire format (NUL). Used to reject
// header injection / response splitting at the application-facing API.
bool containsHeaderInjection(const std::string& s);

class Response {
  public:
    explicit Response(ResponseCompleter completer);

    // JS settable properties
    void setStatus(int32_t code) { m_status = code; }
    auto status() const -> int32_t { return m_status; }

    void setHeader(const std::string& name, const std::string& value);
    auto headers() const -> const std::map<std::string, std::string>& { return m_headers; }

    // JS methods
    void send(const std::string& body);
    void sendJson(v8::Isolate* p_isolate, v8::Local<v8::Value> obj);
    void end();

    // API kept source-compatible with the pre-Drogon streaming surface:
    // writeHead() records the status + headers; write() accumulates body
    // bytes; end() finalizes. Drogon computes Content-Length / chunked
    // framing itself, so everything is buffered until finalize().
    void writeHead(int32_t status, const std::map<std::string, std::string>& headers);

    // Write a chunk. Empty chunk is a no-op. Body bytes are buffered and
    // sent as a single body in finalize().
    void write(const std::vector<uint8_t>& chunk);
    void write(const std::string& chunk);

    auto hasEnded() const -> bool { return m_has_ended; }
    auto headSent() const -> bool { return m_head_sent; }

    // V8 object wrapper. Ownership of `this` is transferred to the JS object
    // and freed via the weak callback once the response has ended AND the JS
    // wrapper is GC'd. Callers must NOT delete the returned object.
    v8::Local<v8::Object> wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    // Template factory
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

  private:
    // V8 weak callback — frees the C++ Response only once it has ended, so an
    // in-flight async response is never freed out from under the sender.
    static void weakCallback(const v8::WeakCallbackInfo<Response>& data);

    // Build the Drogon HttpResponse (status, sanitized headers, body) and
    // hand it to the completer. Idempotent; subsequent calls are no-ops.
    void finalize();

    int32_t m_status = 200;
    std::map<std::string, std::string> m_headers;
    bool m_has_ended = false;
    bool m_head_sent = false;          // True after writeHead() (or finalize()).
    bool m_write_head_called = false;  // True once writeHead() has been invoked.
    std::string m_body;                // Buffered response body.
    ResponseCompleter m_completer;

    static v8::Persistent<v8::ObjectTemplate> m_template;

    // JS property getters/setters
    static void getStatus(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void setStatus(v8::Local<v8::Name> property, v8::Local<v8::Value> value,
                          const v8::PropertyCallbackInfo<void>& info);
    static void getHeaders(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

    // JS methods
    static void sendMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void sendJsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void jsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statusMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void endMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeHeadMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    // setHeader(name, value) — validates and rejects CR/LF/NUL to prevent
    // HTTP response splitting / header injection from app-supplied data.
    static void setHeaderMethod(const v8::FunctionCallbackInfo<v8::Value>& args);

    static Response* unwrap(v8::Local<v8::Object> obj);
};

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_SERVER_RESPONSE_H