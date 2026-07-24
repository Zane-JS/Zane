#ifndef ZANE_BUILTIN_SERVER_RESPONSE_H
#define ZANE_BUILTIN_SERVER_RESPONSE_H

#include "v8.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace zane {
namespace builtin {

// Forward declare connection handle
using SendCallback = std::function<void(int32_t status, const std::map<std::string, std::string>& headers,
                                         const std::vector<uint8_t>& body)>;

// True if `s` contains a character that could terminate / split an HTTP header
// line (CR, LF) or otherwise corrupt the wire format (NUL). Used to reject
// header injection / response splitting at the application-facing API.
bool containsHeaderInjection(const std::string& s);

class Response {
  public:
    explicit Response(SendCallback send_cb);

    // JS settable properties
    void setStatus(int32_t code) { m_status = code; }
    auto status() const -> int32_t { return m_status; }

    void setHeader(const std::string& name, const std::string& value);
    auto headers() const -> const std::map<std::string, std::string>& { return m_headers; }

    // JS methods
    void send(const std::string& body);
    void sendJson(v8::Isolate* p_isolate, v8::Local<v8::Value> obj);
    void end();
    auto hasEnded() const -> bool { return m_has_ended; }

    // V8 object wrapper
    v8::Local<v8::Object> wrap(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    // Template factory
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

  private:
    int32_t m_status = 200;
    std::map<std::string, std::string> m_headers;
    bool m_has_ended = false;
    bool m_headers_sent = false;
    SendCallback m_send_cb;

    static v8::Persistent<v8::ObjectTemplate> m_template;

    // JS property getters/setters
    static void getStatus(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void setStatus(v8::Local<v8::Name> property, v8::Local<v8::Value> value,
                          const v8::PropertyCallbackInfo<void>& info);
    static void getHeaders(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

    // JS methods
    static void sendMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void sendJsonMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void endMethod(const v8::FunctionCallbackInfo<v8::Value>& args);
    // setHeader(name, value) — validates and rejects CR/LF/NUL to prevent
    // HTTP response splitting / header injection from app-supplied data.
    static void setHeaderMethod(const v8::FunctionCallbackInfo<v8::Value>& args);

    static Response* unwrap(v8::Local<v8::Object> obj);
};

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_SERVER_RESPONSE_H
