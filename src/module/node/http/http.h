#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <string>
#include <memory>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace z8 {
namespace module {

class HTTPServer;
class HTTPRequest;
class HTTPResponse;

// HTTP Server using Windows IOCP
class HTTPServer {
public:
    HTTPServer(v8::Isolate* p_isolate);
    ~HTTPServer();
    
    void listen(int32_t port, const std::string& host, v8::Local<v8::Function> callback);
    void close(v8::Local<v8::Function> callback);
    void setRequestHandler(v8::Local<v8::Function> handler);
    
    v8::Isolate* getIsolate() { return p_isolate; }
    
private:
    v8::Isolate* p_isolate;
    v8::Global<v8::Function> m_request_handler;
    v8::Global<v8::Function> m_listen_callback;
    v8::Global<v8::Function> m_close_callback;
    
    bool m_listening;
    int32_t m_port;
    std::string m_host;
    
#ifdef _WIN32
    SOCKET m_listen_socket;
    HANDLE m_iocp;
#endif
};

// HTTP Request
class HTTPRequest {
public:
    HTTPRequest(v8::Isolate* p_isolate);
    v8::Local<v8::Object> toObject();
    
    void setMethod(const std::string& method) { m_method = method; }
    void setUrl(const std::string& url) { m_url = url; }
    void addHeader(const std::string& name, const std::string& value);
    void setBody(const std::string& body) { m_body = body; }
    
private:
    v8::Isolate* p_isolate;
    std::string m_method;
    std::string m_url;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
};

// HTTP Response
class HTTPResponse {
public:
    HTTPResponse(v8::Isolate* p_isolate, SOCKET socket);
    v8::Local<v8::Object> toObject();
    
    void writeHead(int32_t status_code, v8::Local<v8::Object> headers);
    void write(const std::string& data);
    void end(const std::string& data);
    
private:
    v8::Isolate* p_isolate;
    SOCKET m_socket;
    int32_t m_status_code;
    std::map<std::string, std::string> m_headers;
    bool m_headers_sent;
    bool m_finished;
    
    void sendHeaders();
};

// Module interface
class HTTP {
public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
    
    // Server methods
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverListen(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverClose(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Response methods
    static void responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Client methods
    static void request(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void get(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Constants
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    
private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
