/* Z8 HTTP Module - Native node:http implementation with uSockets/IOCP
 * High-performance HTTP server for Windows using native IOCP backend
 */

#include "http.h"
#include "../../../task_queue.h"
#include <cstring>
#include <sstream>
#include <thread>

// uSockets headers
extern "C" {
#include "libusockets.h"
}

namespace z8 {
namespace module {

// Simple HTTP parser
class HTTPParser {
  public:
    static bool parseRequest(const char* data, int length, HTTPRequest* request) {
        std::string str(data, length);
        std::istringstream stream(str);
        std::string line;

        // Parse request line
        if (!std::getline(stream, line))
            return false;

        // Remove \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::istringstream request_line(line);
        std::string method, url, version;
        request_line >> method >> url >> version;

        request->setMethod(method);
        request->setUrl(url);
        request->setHttpVersion(version);

        // Parse headers
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
                break; // End of headers

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);

                // Trim leading spaces
                size_t start = value.find_first_not_of(" \t");
                if (start != std::string::npos)
                    value = value.substr(start);

                request->addHeader(name, value);
            }
        }

        // Body (if any)
        std::string body;
        std::getline(stream, body, '\0');
        if (!body.empty())
            request->setBody(body.c_str(), body.length());

        return true;
    }
};

// HTTPServer implementation
HTTPServer::HTTPServer(v8::Isolate* p_isolate)
    : p_isolate(p_isolate), p_loop(nullptr), p_context(nullptr), p_listen_socket(nullptr), m_listening(false),
      m_port(0) {
    // Use shared global loop (will be created on first use)
    // Don't create loop here - will use global one
}

HTTPServer::~HTTPServer() {
    if (p_listen_socket) {
        us_listen_socket_close(0, p_listen_socket);
    }
    if (p_context) {
        us_socket_context_free(0, p_context);
    }
    // Don't free loop - it's shared
}

// uSockets callbacks
static us_socket_t* on_http_open(us_socket_t* s, int is_client, char* ip, int ip_length) {
    return s;
}

static us_socket_t* on_http_data(us_socket_t* s, char* data, int length) {
    // Get server from socket context
    us_socket_context_t* context = us_socket_context(0, s);
    HTTPServer* server = (HTTPServer*)us_socket_context_ext(0, context);

    if (server) {
        server->handleRequest(s, data, length);
    }

    return s;
}

static us_socket_t* on_http_close(us_socket_t* s, int code, void* reason) {
    return s;
}

void HTTPServer::listen(int port, const char* host, v8::Local<v8::Function> callback) {
    if (m_listening)
        return;

    m_port = port;
    m_host = host; // Store host string
    m_listen_callback.Reset(p_isolate, callback);

    // Create loop if not exists
    if (!p_loop) {
        printf("Creating uSockets loop...\n");
        p_loop = us_create_loop(nullptr, nullptr, nullptr, nullptr, 0);
        if (!p_loop) {
            printf("ERROR: Failed to create uSockets loop!\n");
            return;
        }
        printf("uSockets loop created successfully\n");
    }

    // Create socket context
    printf("Creating socket context...\n");
    us_socket_context_options_t options = {};
    p_context = us_create_socket_context(0, p_loop, sizeof(HTTPServer*), options);
    
    if (!p_context) {
        printf("ERROR: Failed to create socket context!\n");
        return;
    }
    printf("Socket context created\n");

    // Store server pointer in context extension
    *((HTTPServer**)us_socket_context_ext(0, p_context)) = this;

    // Set callbacks
    us_socket_context_on_open(0, p_context, on_http_open);
    us_socket_context_on_data(0, p_context, on_http_data);
    us_socket_context_on_close(0, p_context, on_http_close);

    // Listen
    printf("Attempting to listen on %s:%d...\n", m_host.c_str(), port);
    p_listen_socket = us_socket_context_listen(0, p_context, m_host.c_str(), port, 0, 0);

    if (p_listen_socket) {
        printf("Listen socket created successfully!\n");
        m_listening = true;

        // Call listen callback immediately
        if (!m_listen_callback.IsEmpty()) {
            v8::HandleScope handle_scope(p_isolate);
            v8::Local<v8::Function> cb = m_listen_callback.Get(p_isolate);
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            
            v8::TryCatch try_catch(p_isolate);
            v8::MaybeLocal<v8::Value> result = cb->Call(context, context->Global(), 0, nullptr);
            
            if (try_catch.HasCaught()) {
                // Print error
                v8::String::Utf8Value error(p_isolate, try_catch.Exception());
                printf("Error calling listen callback: %s\n", *error);
            } else if (!result.IsEmpty()) {
                printf("Listen callback called successfully\n");
            }
        } else {
            printf("No listen callback provided\n");
        }

        // Start uSockets loop in background thread
        printf("Starting uSockets IOCP loop in background thread...\n");
        std::thread([this]() {
            printf("IOCP loop thread started\n");
            us_loop_run(p_loop);
            printf("IOCP loop thread ended\n");
        }).detach();

        // Create a keep-alive timer to prevent Z8 event loop from exiting
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        
        v8::Local<v8::Object> global = context->Global();
        v8::Local<v8::Value> set_interval_val = global->Get(context, 
            v8::String::NewFromUtf8(p_isolate, "setInterval").ToLocalChecked()).ToLocalChecked();
        
        if (set_interval_val->IsFunction()) {
            v8::Local<v8::Function> set_interval = set_interval_val.As<v8::Function>();
            
            v8::Local<v8::Function> dummy = v8::Function::New(context, 
                [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                    // Keep loop alive
                }).ToLocalChecked();
            
            v8::Local<v8::Value> argv[2] = {
                dummy,
                v8::Number::New(p_isolate, 1000)
            };
            
            set_interval->Call(context, global, 2, argv).ToLocalChecked();
            printf("Keep-alive timer created\n");
        }
    } else {
        printf("Failed to create listen socket\n");
    }
}

void HTTPServer::close(v8::Local<v8::Function> callback) {
    if (!m_listening)
        return;

    m_close_callback.Reset(p_isolate, callback);

    if (p_listen_socket) {
        us_listen_socket_close(0, p_listen_socket);
        p_listen_socket = nullptr;
    }

    m_listening = false;

    // Call close callback
    if (!m_close_callback.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Function> cb = m_close_callback.Get(p_isolate);
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        cb->Call(context, context->Global(), 0, nullptr).ToLocalChecked();
    }
}

void HTTPServer::setRequestHandler(v8::Local<v8::Function> handler) {
    m_request_handler.Reset(p_isolate, handler);
}

void HTTPServer::handleRequest(us_socket_t* socket, const char* data, int length) {
    if (m_request_handler.IsEmpty())
        return;

    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    // Create request and response objects
    HTTPRequest* request = new HTTPRequest(p_isolate, socket);
    HTTPResponse* response = new HTTPResponse(p_isolate, socket);

    // Parse HTTP request
    if (!HTTPParser::parseRequest(data, length, request)) {
        delete request;
        delete response;
        return;
    }

    v8::Local<v8::Object> req_obj = request->toObject();
    v8::Local<v8::Object> res_obj = response->toObject();

    // Call request handler
    v8::Local<v8::Function> handler = m_request_handler.Get(p_isolate);
    v8::Local<v8::Value> argv[2] = {req_obj, res_obj};
    handler->Call(context, context->Global(), 2, argv).ToLocalChecked();
}

// HTTPRequest implementation
HTTPRequest::HTTPRequest(v8::Isolate* p_isolate, us_socket_t* socket) : p_isolate(p_isolate), p_socket(socket) {}

v8::Local<v8::Object> HTTPRequest::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);

    v8::Local<v8::Object> obj = tmpl->NewInstance(context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));

    // Set properties
    obj->Set(context, v8::String::NewFromUtf8(p_isolate, "method").ToLocalChecked(),
             v8::String::NewFromUtf8(p_isolate, m_method.c_str()).ToLocalChecked())
        .Check();

    obj->Set(context, v8::String::NewFromUtf8(p_isolate, "url").ToLocalChecked(),
             v8::String::NewFromUtf8(p_isolate, m_url.c_str()).ToLocalChecked())
        .Check();

    obj->Set(context, v8::String::NewFromUtf8(p_isolate, "httpVersion").ToLocalChecked(),
             v8::String::NewFromUtf8(p_isolate, m_http_version.c_str()).ToLocalChecked())
        .Check();

    // Headers object
    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    for (const auto& pair : m_headers) {
        headers
            ->Set(context, v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked(),
                  v8::String::NewFromUtf8(p_isolate, pair.second.c_str()).ToLocalChecked())
            .Check();
    }
    obj->Set(context, v8::String::NewFromUtf8(p_isolate, "headers").ToLocalChecked(), headers).Check();

    return handle_scope.Escape(obj);
}

void HTTPRequest::addHeader(const std::string& name, const std::string& value) {
    m_headers[name] = value;
}

void HTTPRequest::setBody(const char* data, size_t length) {
    m_body.assign(data, length);
}

// HTTPResponse implementation
HTTPResponse::HTTPResponse(v8::Isolate* p_isolate, us_socket_t* socket)
    : p_isolate(p_isolate), p_socket(socket), m_status_code(200), m_headers_sent(false), m_finished(false) {}

v8::Local<v8::Object> HTTPResponse::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);

    // Add methods
    tmpl->Set(p_isolate, "writeHead", v8::FunctionTemplate::New(p_isolate, HTTP::responseWriteHead));
    tmpl->Set(p_isolate, "write", v8::FunctionTemplate::New(p_isolate, HTTP::responseWrite));
    tmpl->Set(p_isolate, "end", v8::FunctionTemplate::New(p_isolate, HTTP::responseEnd));

    v8::Local<v8::Object> obj = tmpl->NewInstance(context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));

    return handle_scope.Escape(obj);
}

void HTTPResponse::writeHead(int status_code, v8::Local<v8::Object> headers) {
    if (m_headers_sent)
        return;

    m_status_code = status_code;

    // Parse headers
    if (!headers.IsEmpty()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Array> props = headers->GetPropertyNames(context).ToLocalChecked();

        for (uint32_t i = 0; i < props->Length(); i++) {
            v8::Local<v8::Value> key = props->Get(context, i).ToLocalChecked();
            v8::Local<v8::Value> value = headers->Get(context, key).ToLocalChecked();

            v8::String::Utf8Value key_str(p_isolate, key);
            v8::String::Utf8Value value_str(p_isolate, value);

            m_headers[*key_str] = *value_str;
        }
    }

    sendHeaders();
}

void HTTPResponse::write(const char* data, size_t length) {
    if (!m_headers_sent)
        sendHeaders();

    if (m_finished)
        return;

    us_socket_write(0, p_socket, data, (int)length, 0);
}

void HTTPResponse::end(const char* data, size_t length) {
    if (m_finished)
        return;

    if (!m_headers_sent)
        sendHeaders();

    if (data && length > 0) {
        us_socket_write(0, p_socket, data, (int)length, 0);
    }

    m_finished = true;
    us_socket_shutdown(0, p_socket);
}

void HTTPResponse::sendHeaders() {
    if (m_headers_sent)
        return;

    std::ostringstream ss;

    // Status line
    ss << "HTTP/1.1 " << m_status_code << " ";
    switch (m_status_code) {
    case 200:
        ss << "OK";
        break;
    case 404:
        ss << "Not Found";
        break;
    case 500:
        ss << "Internal Server Error";
        break;
    default:
        ss << "Unknown";
        break;
    }
    ss << "\r\n";

    // Headers
    for (const auto& pair : m_headers) {
        ss << pair.first << ": " << pair.second << "\r\n";
    }

    // End of headers
    ss << "\r\n";

    std::string headers_str = ss.str();
    us_socket_write(0, p_socket, headers_str.c_str(), (int)headers_str.length(), 0);

    m_headers_sent = true;
}

// HTTP module interface
v8::Local<v8::ObjectTemplate> HTTP::createTemplate(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    // createServer method
    tmpl->Set(p_isolate, "createServer", v8::FunctionTemplate::New(p_isolate, createServer));

    return handle_scope.Escape(tmpl);
}

void HTTP::createServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    // Create server instance
    HTTPServer* server = new HTTPServer(isolate);

    // Get request handler if provided
    if (args.Length() > 0 && args[0]->IsFunction()) {
        server->setRequestHandler(args[0].As<v8::Function>());
    }

    // Create server object
    v8::Local<v8::ObjectTemplate> server_tmpl = v8::ObjectTemplate::New(isolate);
    server_tmpl->SetInternalFieldCount(1);
    server_tmpl->Set(isolate, "listen", v8::FunctionTemplate::New(isolate, serverListen));
    server_tmpl->Set(isolate, "close", v8::FunctionTemplate::New(isolate, serverClose));

    v8::Local<v8::Object> server_obj = server_tmpl->NewInstance(context).ToLocalChecked();
    server_obj->SetInternalField(0, v8::External::New(isolate, server));

    args.GetReturnValue().Set(server_obj);
}

void HTTP::serverListen(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    HTTPServer* p_server = unwrapServer(args.This());
    if (!p_server)
        return;

    // Parse arguments: port, [host], [callback]
    int32_t port = 3000;
    std::string host_str = "0.0.0.0";
    v8::Local<v8::Function> callback;

    if (args.Length() > 0 && args[0]->IsNumber()) {
        port = args[0]->Int32Value(p_isolate->GetCurrentContext()).ToChecked();
    }

    if (args.Length() > 1 && args[1]->IsString()) {
        v8::String::Utf8Value host_val(p_isolate, args[1]);
        host_str = std::string(*host_val, host_val.length());
    }

    if (args.Length() > 0 && args[args.Length() - 1]->IsFunction()) {
        callback = args[args.Length() - 1].As<v8::Function>();
    }

    p_server->listen(port, host_str.c_str(), callback);

    args.GetReturnValue().Set(args.This());
}

void HTTP::serverClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    HTTPServer* p_server = unwrapServer(args.This());
    if (!p_server)
        return;

    v8::Local<v8::Function> callback;
    if (args.Length() > 0 && args[0]->IsFunction()) {
        callback = args[0].As<v8::Function>();
    }

    p_server->close(callback);
}

void HTTP::responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    HTTPResponse* p_response = unwrapResponse(args.This());
    if (!p_response)
        return;

    int32_t status_code = 200;
    v8::Local<v8::Object> headers;

    if (args.Length() > 0 && args[0]->IsNumber()) {
        status_code = args[0]->Int32Value(p_isolate->GetCurrentContext()).ToChecked();
    }

    if (args.Length() > 1 && args[1]->IsObject()) {
        headers = args[1].As<v8::Object>();
    }

    p_response->writeHead(status_code, headers);
}

void HTTP::responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    HTTPResponse* p_response = unwrapResponse(args.This());
    if (!p_response)
        return;

    if (args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value data(p_isolate, args[0]);
        p_response->write(*data, data.length());
    }
}

void HTTP::responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    HTTPResponse* p_response = unwrapResponse(args.This());
    if (!p_response)
        return;

    const char* p_data = nullptr;
    size_t length = 0;

    if (args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value data_str(p_isolate, args[0]);
        p_data = *data_str;
        length = data_str.length();
    }

    p_response->end(p_data, length);
}

void HTTP::requestOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Placeholder for event handling
}

HTTPServer* HTTP::unwrapServer(v8::Local<v8::Object> obj) {
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::Data> field = obj->GetInternalField(0);
        if (!field.IsEmpty() && field->IsValue()) {
            v8::Local<v8::Value> value = field.As<v8::Value>();
            if (value->IsExternal()) {
                return static_cast<HTTPServer*>(value.As<v8::External>()->Value());
            }
        }
    }
    return nullptr;
}

HTTPResponse* HTTP::unwrapResponse(v8::Local<v8::Object> obj) {
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::Data> field = obj->GetInternalField(0);
        if (!field.IsEmpty() && field->IsValue()) {
            v8::Local<v8::Value> value = field.As<v8::Value>();
            if (value->IsExternal()) {
                return static_cast<HTTPResponse*>(value.As<v8::External>()->Value());
            }
        }
    }
    return nullptr;
}

HTTPRequest* HTTP::unwrapRequest(v8::Local<v8::Object> obj) {
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::Data> field = obj->GetInternalField(0);
        if (!field.IsEmpty() && field->IsValue()) {
            v8::Local<v8::Value> value = field.As<v8::Value>();
            if (value->IsExternal()) {
                return static_cast<HTTPRequest*>(value.As<v8::External>()->Value());
            }
        }
    }
    return nullptr;
}

} // namespace module
} // namespace z8
