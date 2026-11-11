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

// Streaming callbacks. All run on the connection's event-loop thread; the
// dispatch is already done by the server-side lambda that owns the connection
// shared_ptr, so these are plain synchronous calls from the JS thread's POV.
//
// - writeHeadCb: flush status line + headers immediately. After this returns
//   the response is in streaming mode and subsequent writeChunkCb calls
//   produce body chunks; endChunkedCb terminates the chunked body.
// - writeChunkCb: append one chunked-encoding frame to the connection.
//   The bytes are framed as <hex-size>\r\n<data>\r\n per RFC 7230 §4.1.
// - endChunkedCb: append the chunked-encoding terminator ("0\r\n\r\n")
//   and close out the response. Idempotent; subsequent write() / end()
//   become no-ops just like the legacy one-shot send() path.
using WriteHeadCallback = std::function<void(int32_t status, const std::map<std::string, std::string>& headers)>;
using WriteChunkCallback = std::function<void(const std::vector<uint8_t>& chunk)>;
using EndChunkedCallback = std::function<void()>;

// True if `s` contains a character that could terminate / split an HTTP header
// line (CR, LF) or otherwise corrupt the wire format (NUL). Used to reject
// header injection / response splitting at the application-facing API.
bool containsHeaderInjection(const std::string& s);

class Response {
  public:
    // Construct with the legacy one-shot send callback. Streaming callbacks
    // default to null; the server fills them in when it wires up the
    // Response. Keeping the old 1-arg constructor means existing call sites
    // and tests stay source-compatible.
    explicit Response(SendCallback send_cb);

    // Streaming constructor: server.cpp uses this so the response can do
    // chunked transfer encoding. send_cb may be empty (we only need it for
    // legacy non-streaming send), but in practice the server always passes
    // all four so we have a complete toolkit.
    Response(SendCallback send_cb,
             WriteHeadCallback write_head_cb,
             WriteChunkCallback write_chunk_cb,
             EndChunkedCallback end_chunked_cb);

    // JS settable properties
    void setStatus(int32_t code) { m_status = code; }
    auto status() const -> int32_t { return m_status; }

    void setHeader(const std::string& name, const std::string& value);
    auto headers() const -> const std::map<std::string, std::string>& { return m_headers; }

    // JS methods
    void send(const std::string& body);
    void sendJson(v8::Isolate* p_isolate, v8::Local<v8::Value> obj);
    void end();

    // Streaming API. writeHead() flushes the response head (status +
    // headers) immediately and transitions the response into streaming mode.
    // After writeHead(), the JS app can call write(chunk) any number of
    // times followed by end() to terminate. If writeHead() is not called
    // explicitly, the first send() / write() / end() auto-flushes a
    // chunked head for backward-compatibility with the legacy one-shot API.
    //
    // Headers argument may be empty; if non-empty it REPLACES the headers
    // collected via setHeader() up to that point. This mirrors Node.js
    // http.ServerResponse.writeHead(status, headers).
    void writeHead(int32_t status, const std::map<std::string, std::string>& headers);

    // Write a chunk. If the head hasn't been flushed yet (no explicit
    // writeHead() and no prior send()), auto-flushes a chunked head first
    // so the chunk bytes are valid. Empty chunk is a no-op (do NOT emit a
    // 0-length chunked frame mid-stream — that's the terminator).
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

    // Internal: ensure the response head has been flushed. No-op if already
    // sent or if the response is streaming with no callback available.
    void flushHeadIfNeeded();

    int32_t m_status = 200;
    std::map<std::string, std::string> m_headers;
    bool m_has_ended = false;
    bool m_head_sent = false;          // True after writeHead() or auto-flush on first send/write.
    bool m_write_head_called = false;  // True once writeHead() has been invoked.
    bool m_streaming = false;          // True once writeHead() OR write() has been used; locks response into chunked mode.
    SendCallback m_send_cb;
    WriteHeadCallback m_write_head_cb;
    WriteChunkCallback m_write_chunk_cb;
    EndChunkedCallback m_end_chunked_cb;

    static v8::Persistent<v8::ObjectTemplate> m_template;

    // Persistent handle to the JS wrapper.
    //
    // It must live on the object (not in a local inside wrap()): V8 requires a
    // weak callback to reset the handle it fired for, and destroying the
    // handle outside the callback trips
    // "Handle not reset in first callback".
    v8::Global<v8::Object> m_self;

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
