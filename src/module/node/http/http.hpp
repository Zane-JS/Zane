#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <atomic>

// Include Trantor (Networking)
#include <trantor/net/TcpServer.h>
#include <trantor/net/EventLoopThread.h>

// Include llhttp (Parsing)
#include "llhttp.h"

namespace z8 {
namespace module {

class HTTPServer;
class HTTPRequest;
class HTTPResponse;

// HTTP Server using Trantor (TCP) + llhttp (Parsing)
class HTTPServer {
public:
    HTTPServer(v8::Isolate* p_isolate);
    ~HTTPServer();
    
    void listen(int32_t port, const std::string& host, v8::Local<v8::Function> callback);
    void close(v8::Local<v8::Function> callback);
    void setRequestHandler(v8::Local<v8::Function> handler);
    
    v8::Isolate* getIsolate() { return p_isolate; }
    
    static bool hasActiveServers() { return m_active_servers > 0; }
    
private:
    static std::atomic<int32_t> m_active_servers;
    v8::Isolate* p_isolate;
    v8::Global<v8::Function> m_request_handler;
    
    bool m_listening;
    int32_t m_port;
    std::string m_host;
    
    std::unique_ptr<trantor::TcpServer> up_tcp_server;
    trantor::EventLoopThread m_loop_thread;
};

// HTTP Request using llhttp
class HTTPRequest {
public:
    HTTPRequest(v8::Isolate* p_isolate);
    ~HTTPRequest();
    
    v8::Local<v8::Object> toObject();
    
    llhttp_errno_t parse(const char* data, size_t length);
    
    void setMethod(const std::string& method) { m_method = method; }
    void setUrl(const std::string& url) { m_url = url; }
    void addHeader(const std::string& name, const std::string& value);
    void appendBody(const char* data, size_t length);
    
    const std::string& getMethod() const { return m_method; }
    const std::string& getUrl() const { return m_url; }
    
    // Parsing state
    std::string m_current_header_field;
    std::string m_current_header_value;
    bool m_parsing_complete;

private:
    v8::Isolate* p_isolate;
    std::string m_method;
    std::string m_url;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
    
    llhttp_t m_parser;
    llhttp_settings_t m_settings;
};

// HTTP Response using Trantor::TcpConnection
class HTTPResponse {
public:
    HTTPResponse(v8::Isolate* p_isolate, const trantor::TcpConnectionPtr& conn);
    v8::Local<v8::Object> toObject();
    
    void writeHead(int32_t status_code, v8::Local<v8::Object> headers);
    void write(const std::string& data);
    void end(const std::string& data);
    
private:
    v8::Isolate* p_isolate;
    trantor::TcpConnectionPtr m_conn;
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
    
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverListen(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverClose(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    static void responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
