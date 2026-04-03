#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

// Include Trantor (Networking)
#include <trantor/net/TcpServer.h>
#include <trantor/net/TcpClient.h>
#include <trantor/net/EventLoopThread.h>

// Include llhttp (Parsing)
#include "llhttp.h"

namespace z8 {
namespace module {

class HTTPServer;
class HTTPRequest;
class HTTPResponse;
class HTTPClientRequest;

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
    enum class HttpEventType {
        HEADERS,
        BODY_CHUNK,
        MESSAGE_COMPLETE
    };

    struct HttpEvent {
        HttpEventType m_type;
        std::string m_data;
    };

    HTTPRequest(v8::Isolate* p_isolate);
    ~HTTPRequest();
    
    v8::Local<v8::Object> toObject();
    void setJSObject(v8::Local<v8::Object> obj);

    void pushEvent(HttpEventType type, const std::string& data = "");
    std::vector<HttpEvent> popEvents();
    
    llhttp_errno_t parse(const char* data, size_t length);
    
    void setMethod(const std::string& method) { m_method = method; }
    void appendUrl(const char* p_data, size_t length);
    void appendHeaderField(const char* p_data, size_t length);
    void appendHeaderValue(const char* p_data, size_t length);
    void finishPendingHeader();
    void addHeader(const std::string& name, const std::string& value);

    const std::string& getMethod() const { return m_method; }
    const std::string& getUrl() const { return m_url; }

    std::string m_current_header_field;
    std::string m_current_header_value;
    bool m_parsing_complete;
    uint8_t m_http_major;
    uint8_t m_http_minor;

private:
    enum class HeaderParseState {
        NONE,
        FIELD,
        VALUE
    };

    v8::Isolate* p_isolate;
    v8::Global<v8::Object> m_req_obj;
    std::string m_method;
    std::string m_url;
    std::map<std::string, std::vector<std::string>> m_header_values;
    std::vector<std::string> m_raw_headers;
    HeaderParseState m_last_header_state;
    
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

    bool getHeadersSent() const { return m_headers_sent; }
    bool getFinished() const { return m_finished; }
     
    int32_t getStatusCode() const { return m_status_code; }
    void setStatusCode(int32_t code) { m_status_code = code; }
    std::string getStatusMessage() const { return m_status_message; }
    void setStatusMessage(const std::string& msg) { m_status_message = msg; }
    
private:
    v8::Isolate* p_isolate;
    trantor::TcpConnectionPtr m_conn;
    v8::Global<v8::Object> m_res_obj;
    int32_t m_status_code;
    std::string m_status_message;
    std::map<std::string, std::string> m_headers;
    bool m_headers_sent;
    bool m_finished;
    bool m_use_chunked_encoding;
     
    void sendHeaders();
    void sendChunk(const std::string& data);
    void emit(const char* p_event_name);
};

class HTTPClientRequest {
public:
    HTTPClientRequest(v8::Isolate* p_isolate,
                      const std::string& method,
                      const std::string& host,
                      int32_t port,
                      const std::string& path,
                      v8::Local<v8::Object> headers,
                      v8::Local<v8::Function> callback);
    ~HTTPClientRequest();

    v8::Local<v8::Object> createObject(v8::Isolate* p_isolate);
    void execute();
    void emitResponse();
    void emitData(const std::string& data);
    void emitEnd();
    void appendResponseHeaderField(const char* p_data, size_t length);
    void appendResponseHeaderValue(const char* p_data, size_t length);
    void finishPendingResponseHeader();

    static void write(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void end(const v8::FunctionCallbackInfo<v8::Value>& args);

    static int32_t onMessageBegin(llhttp_t* p_parser);
    static int32_t onStatus(llhttp_t* p_parser, const char* p_at, size_t length);
    static int32_t onHeaderField(llhttp_t* p_parser, const char* p_at, size_t length);
    static int32_t onHeaderValue(llhttp_t* p_parser, const char* p_at, size_t length);
    static int32_t onHeadersComplete(llhttp_t* p_parser);
    static int32_t onBody(llhttp_t* p_parser, const char* p_at, size_t length);
    static int32_t onMessageComplete(llhttp_t* p_parser);

private:
    enum class HeaderParseState {
        NONE,
        FIELD,
        VALUE
    };

    v8::Isolate* p_isolate;
    std::string m_method;
    std::string m_host;
    int32_t m_port;
    std::string m_path;
    std::string m_body;
    std::map<std::string, std::string> m_headers;

    int32_t m_status_code;
    std::string m_status_message;
    std::string m_current_header_field;
    std::string m_current_header_value;
    HeaderParseState m_last_header_state;
    std::vector<std::string> m_raw_response_headers;

    llhttp_t m_parser;
    llhttp_settings_t m_settings;
    std::shared_ptr<trantor::TcpClient> sp_tcp_client;
    std::unique_ptr<trantor::EventLoopThread> up_loop_thread;

    v8::Global<v8::Object> m_js_object;
    v8::Global<v8::Object> m_response_obj;
    v8::Global<v8::Function> m_response_callback;
};

class HTTP {
public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
     
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverListen(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverClose(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void request(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void get(const v8::FunctionCallbackInfo<v8::Value>& args);
     
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
    static void responseGetHeadersSent(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseGetFinished(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
     
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
