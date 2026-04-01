#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

// Forward declarations for uSockets
struct us_loop_t;
struct us_socket_context_t;
struct us_listen_socket_t;
struct us_socket_t;

namespace z8 {
namespace module {

class HTTPServer;
class HTTPRequest;
class HTTPResponse;

// HTTP Server class
class HTTPServer {
  public:
    HTTPServer(v8::Isolate* p_isolate);
    ~HTTPServer();

    void listen(int port, const char* host, v8::Local<v8::Function> callback);
    void close(v8::Local<v8::Function> callback);

    v8::Isolate* getIsolate() { return p_isolate; }
    us_loop_t* getLoop() { return p_loop; }

    void setRequestHandler(v8::Local<v8::Function> handler);
    void handleRequest(us_socket_t* socket, const char* data, int length);

  private:
    v8::Isolate* p_isolate;
    us_loop_t* p_loop;
    us_socket_context_t* p_context;
    us_listen_socket_t* p_listen_socket;

    v8::Global<v8::Function> m_request_handler;
    v8::Global<v8::Function> m_listen_callback;
    v8::Global<v8::Function> m_close_callback;

    bool m_listening;
    int m_port;
};

// HTTP Request class
class HTTPRequest {
  public:
    HTTPRequest(v8::Isolate* p_isolate, us_socket_t* socket);

    v8::Local<v8::Object> toObject();

    void setMethod(const std::string& method) { m_method = method; }
    void setUrl(const std::string& url) { m_url = url; }
    void setHttpVersion(const std::string& version) { m_http_version = version; }
    void addHeader(const std::string& name, const std::string& value);
    void setBody(const char* data, size_t length);

    const std::string& getMethod() const { return m_method; }
    const std::string& getUrl() const { return m_url; }

  private:
    v8::Isolate* p_isolate;
    us_socket_t* p_socket;

    std::string m_method;
    std::string m_url;
    std::string m_http_version;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
};

// HTTP Response class
class HTTPResponse {
  public:
    HTTPResponse(v8::Isolate* p_isolate, us_socket_t* socket);

    v8::Local<v8::Object> toObject();

    void writeHead(int status_code, v8::Local<v8::Object> headers);
    void write(const char* data, size_t length);
    void end(const char* data, size_t length);

    bool isFinished() const { return m_finished; }

  private:
    v8::Isolate* p_isolate;
    us_socket_t* p_socket;

    int m_status_code;
    std::map<std::string, std::string> m_headers;
    bool m_headers_sent;
    bool m_finished;

    void sendHeaders();
};

// Module interface
class HTTP {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    // Factory methods
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Server methods (called on server instance)
    static void serverListen(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverClose(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Response methods (called on response instance)
    static void responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Request methods (called on request instance)
    static void requestOn(const v8::FunctionCallbackInfo<v8::Value>& args);

  private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
    static HTTPRequest* unwrapRequest(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
