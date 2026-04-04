#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"
#include <cstdint>
#include <map>
#include <string>
#include <memory>
#include <set>
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
    void closeAllConnections();
    void setRequestHandler(v8::Local<v8::Function> handler);
    void setJSObject(v8::Local<v8::Object> obj);
    void retain();
    void release();
    void markGcPending() { m_gc_pending = true; }
    void markDisposed();
     
    v8::Isolate* getIsolate() { return p_isolate; }
    bool getListening() const { return m_listening; }
    bool isDisposed() const { return m_disposed; }
     
    static bool hasActiveServers() { return m_active_servers > 0; }
     
private:
    static std::atomic<int32_t> m_active_servers;
    v8::Isolate* p_isolate;
    v8::Global<v8::Object> m_server_obj;
    v8::Global<v8::Function> m_request_handler;
    
    std::atomic<bool> m_listening;
    bool m_disposed;
    std::atomic<bool> m_gc_pending;
    std::atomic<int32_t> m_ref_count;
    std::atomic<bool> m_delete_scheduled;
    int32_t m_port;
    std::string m_host;
    std::mutex m_connections_mutex;
    std::set<trantor::TcpConnectionPtr> m_connections;
    
    trantor::EventLoopThread m_loop_thread;
    std::unique_ptr<trantor::TcpServer> up_tcp_server;

    void scheduleDelete();
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
    bool appendBodyChunk(const char* p_data, size_t length);
    void setConnection(const trantor::TcpConnectionPtr& conn);
    void markDestroyed();
    bool isDestroyed() const { return m_destroyed; }
    bool shouldCloseConnection() const;
    void destroy();

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
    trantor::TcpConnectionPtr m_conn;
    bool m_destroyed;
    size_t m_header_count;
    size_t m_body_size;
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
    ~HTTPResponse();
    v8::Local<v8::Object> toObject();
     
    void writeHead(int32_t status_code, v8::Local<v8::Object> headers);
    void writeHead(int32_t status_code, const std::string& status_message, v8::Local<v8::Object> headers);
    void setHeader(const std::string& name, const std::string& value);
    void setHeader(const std::string& name, const std::vector<std::string>& values);
    bool hasHeader(const std::string& name);
    const std::vector<std::string>* getHeader(const std::string& name) const;
    void removeHeader(const std::string& name);
    v8::Local<v8::Array> getHeaderNames();
    v8::Local<v8::Object> getHeaders();
    void write(const std::string& data);
    void end(const std::string& data);
    void flushHeaders();

    bool getHeadersSent() const { return m_headers_sent; }
    bool getFinished() const { return m_finished; }
     
    int32_t getStatusCode() const { return m_status_code; }
    void setStatusCode(int32_t code) { m_status_code = code; }
    std::string getStatusMessage() const { return m_status_message; }
    void setStatusMessage(const std::string& msg) { m_status_message = msg; }
    bool getSendDate() const { return m_send_date; }
    void setSendDate(bool send_date) { m_send_date = send_date; }
    void retain();
    void release();
    void markGcPending() { m_gc_pending = true; }
     
private:
    v8::Isolate* p_isolate;
    trantor::TcpConnectionPtr m_conn;
    v8::Global<v8::Object> m_res_obj;
    int32_t m_status_code;
    std::string m_status_message;
    std::map<std::string, std::vector<std::string>> m_headers;
    bool m_headers_sent;
    bool m_finished;
    bool m_use_chunked_encoding;
    bool m_send_date;
    std::atomic<bool> m_gc_pending;
    std::atomic<int32_t> m_ref_count;
    std::atomic<bool> m_delete_scheduled;
     
    void sendHeaders();
    void sendChunk(const std::string& data);
    void emit(const char* p_event_name);
    std::string getFirstHeaderValue(const std::string& name) const;
    void scheduleDelete();
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
    void emitClose();
    void destroyRequest();
    void setTimeout(int32_t timeout_ms);
    void execute(bool headers_only);
    void setHeader(const std::string& name, const std::string& value);
    bool hasHeader(const std::string& name) const;
    std::string getHeader(const std::string& name) const;
    void removeHeader(const std::string& name);
    std::vector<std::string> getHeaderNames() const;
    std::vector<std::string> getRawHeaderNames() const;
    const std::map<std::string, std::string>& getHeaders() const { return m_headers; }
    bool getFinished() const { return m_finished; }
    bool getExecuted() const { return m_executed; }
    bool isComplete() const { return m_request_complete; }
    bool isDestroyed() const { return m_destroyed; }
    void retain();
    void release();
    void markGcPending() { m_gc_pending = true; }
    void appendResponseHeaderField(const char* p_data, size_t length);
    void appendResponseHeaderValue(const char* p_data, size_t length);
    void finishPendingResponseHeader();

    static void write(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void end(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void destroy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void flushHeaders(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setTimeout(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void hasHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void removeHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getHeaders(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getRawHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args);

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
    std::vector<std::string> m_raw_header_names;

    int32_t m_status_code;
    std::string m_status_message;
    std::string m_current_header_field;
    std::string m_current_header_value;
    HeaderParseState m_last_header_state;
    std::vector<std::string> m_raw_response_headers;

    llhttp_t m_parser;
    llhttp_settings_t m_settings;
    std::shared_ptr<trantor::TcpClient> sp_tcp_client;
    trantor::TcpConnectionPtr m_connection;
    std::unique_ptr<trantor::EventLoopThread> up_loop_thread;
    bool m_finished;
    bool m_executed;
    bool m_request_complete;
    bool m_connection_ready;
    bool m_request_headers_sent;
    bool m_request_chunked;
    bool m_end_requested;
    bool m_request_body_finalized;
    std::atomic<bool> m_gc_pending;
    std::atomic<bool> m_network_ref_active;
    std::atomic<int32_t> m_ref_count;
    std::atomic<bool> m_delete_scheduled;
    bool m_destroyed;
    int32_t m_timeout_ms;
    std::recursive_mutex m_request_mutex;

    v8::Global<v8::Object> m_js_object;
    v8::Global<v8::Object> m_response_obj;
    v8::Global<v8::Function> m_response_callback;
    bool m_error_emitted;
    bool m_close_emitted;

    void releaseNetworkReference();
    void scheduleDelete();
    void sendRequestHeaders();
    void sendRequestBodyData(const std::string& data);
    void flushPendingRequestBody();
    void finalizeRequestBody();
};

class HTTP {
public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
     
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverListen(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverClose(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void serverCloseAllConnections(const v8::FunctionCallbackInfo<v8::Value>& args);

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
    static void responseFlushHeaders(const v8::FunctionCallbackInfo<v8::Value>& args);
     
    // JS Accessors
    static void responseGetStatusCode(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseSetStatusCode(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    static void responseGetStatusMessage(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseSetStatusMessage(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    static void responseGetHeadersSent(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseGetFinished(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseGetSendDate(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void responseSetSendDate(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    static void serverGetListening(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void requestDestroy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void requestGetDestroyed(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
      
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

private:
    static HTTPServer* unwrapServer(v8::Local<v8::Object> obj);
    static HTTPRequest* unwrapRequest(v8::Local<v8::Object> obj);
    static HTTPResponse* unwrapResponse(v8::Local<v8::Object> obj);
    static HTTPClientRequest* unwrapClientRequest(v8::Local<v8::Object> obj);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
