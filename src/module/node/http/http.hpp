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
    void setJSObject(v8::Local<v8::Object> obj);
    
    // Event structures for passing from I/O thread to Main thread
    enum class HttpEventType { HEADERS, BODY_CHUNK, MESSAGE_COMPLETE };
    struct HttpEvent {
        HttpEventType m_type;
        std::string m_data; // Used for BODY_CHUNK
    };
    
    void pushEvent(HttpEventType type, const std::string& data = "");
    std::vector<HttpEvent> popEvents();
    
    llhttp_errno_t parse(const char* data, size_t length);
    
    void setMethod(const std::string& method) { m_method = method; }
    void setUrl(const std::string& url) { m_url = url; }
    void addHeader(const std::string& name, const std::string& value);
    
    // JS support
    
    const std::string& getMethod() const { return m_method; }
    const std::string& getUrl() const { return m_url; }
    
    // Parsing state
    std::string m_current_header_field;
    std::string m_current_header_value;
    bool m_parsing_complete;
    bool m_headers_sent;
    uint8_t m_http_major;
    uint8_t m_http_minor;

private:
    v8::Isolate* p_isolate;
    v8::Global<v8::Object> m_req_obj;
    std::string m_method;
    std::string m_url;
    std::map<std::string, std::string> m_headers;
    std::vector<std::string> m_raw_headers;
    
    std::mutex m_events_mutex;
    std::vector<HttpEvent> m_pending_events;
    
    llhttp_t m_parser;
    llhttp_settings_t m_settings;
};

// HTTP Response using Trantor::TcpConnection
class HTTPResponse {
public:
    HTTPResponse(v8::Isolate* p_isolate, const trantor::TcpConnectionPtr& conn);
    v8::Local<v8::Object> toObject();
    
    void writeHead(int32_t status_code, v8::Local<v8::Object> headers);
    void writeHead(int32_t status_code, const std::string& status_message, v8::Local<v8::Object> headers);
    void setHeader(const std::string& name, const std::string& value);
    bool hasHeader(const std::string& name);
    std::string getHeader(const std::string& name);
    void removeHeader(const std::string& name);
    v8::Local<v8::Array> getHeaderNames();
    v8::Local<v8::Object> getHeaders();
    void write(const std::string& data);
    void end(const std::string& data);
    
    int32_t getStatusCode() const { return m_status_code; }
    void setStatusCode(int32_t code) { m_status_code = code; }
    std::string getStatusMessage() const { return m_status_message; }
    void setStatusMessage(const std::string& msg) { m_status_message = msg; }
    
private:
    v8::Isolate* p_isolate;
    trantor::TcpConnectionPtr m_conn;
    int32_t m_status_code;
    std::string m_status_message;
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
    static void responseSetHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseGetHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseHasHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseRemoveHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseGetHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseGetHeaders(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // JS Accessors
    static void responseGetStatusCode(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseSetStatusCode(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    static void responseGetStatusMessage(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseSetStatusMessage(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
