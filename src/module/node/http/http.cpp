#include "http.h"
#include <sstream>
#include <thread>
#include <iostream>

namespace z8 {
namespace module {

// HTTPServer implementation
HTTPServer::HTTPServer(v8::Isolate* p_isolate)
    : p_isolate(p_isolate), m_listening(false), m_port(0)
#ifdef _WIN32
    , m_listen_socket(INVALID_SOCKET), m_iocp(NULL)
#endif
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

HTTPServer::~HTTPServer() {
#ifdef _WIN32
    if (m_listen_socket != INVALID_SOCKET) {
        closesocket(m_listen_socket);
    }
    if (m_iocp != NULL) {
        CloseHandle(m_iocp);
    }
    WSACleanup();
#endif
}

void HTTPServer::listen(int32_t port, const std::string& host, v8::Local<v8::Function> callback) {
    if (m_listening) return;
    
    m_port = port;
    m_host = host;
    if (!callback.IsEmpty()) {
        m_listen_callback.Reset(p_isolate, callback);
    }
    
#ifdef _WIN32
    // Create socket
    m_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_socket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return;
    }
    
    // Set socket options
    int32_t opt = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    // Bind
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    if (bind(m_listen_socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket" << std::endl;
        closesocket(m_listen_socket);
        m_listen_socket = INVALID_SOCKET;
        return;
    }
    
    // Listen
    if (::listen(m_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Failed to listen" << std::endl;
        closesocket(m_listen_socket);
        m_listen_socket = INVALID_SOCKET;
        return;
    }
    
    m_listening = true;
    
    // Call listen callback
    if (!m_listen_callback.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Function> cb = m_listen_callback.Get(p_isolate);
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        cb->Call(context, context->Global(), 0, nullptr).ToLocalChecked();
    }
    
    // Start accept thread (simplified - not using IOCP for now)
    std::thread([this]() {
        while (m_listening) {
            SOCKET client_socket = accept(m_listen_socket, NULL, NULL);
            if (client_socket == INVALID_SOCKET) break;
            
            // Handle request in separate thread
            std::thread([this, client_socket]() {
                char buffer[4096];
                int32_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes_received > 0) {
                    buffer[bytes_received] = '\0';
                    
                    // Parse HTTP request (simple parser)
                    std::string request_str(buffer, bytes_received);
                    std::istringstream stream(request_str);
                    std::string line;
                    
                    // Parse request line
                    std::getline(stream, line);
                    std::istringstream request_line(line);
                    std::string method, url, version;
                    request_line >> method >> url >> version;
                    
                    // Create request and response objects
                    HTTPRequest* request = new HTTPRequest(p_isolate);
                    request->setMethod(method);
                    request->setUrl(url);
                    
                    // Parse headers
                    while (std::getline(stream, line) && line != "\r") {
                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            std::string name = line.substr(0, colon);
                            std::string value = line.substr(colon + 1);
                            // Trim
                            value.erase(0, value.find_first_not_of(" \t\r\n"));
                            value.erase(value.find_last_not_of(" \t\r\n") + 1);
                            request->addHeader(name, value);
                        }
                    }
                    
                    HTTPResponse* response = new HTTPResponse(p_isolate, client_socket);
                    
                    // Call request handler
                    if (!m_request_handler.IsEmpty()) {
                        v8::Locker locker(p_isolate);
                        v8::Isolate::Scope isolate_scope(p_isolate);
                        v8::HandleScope handle_scope(p_isolate);
                        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                        v8::Context::Scope context_scope(context);
                        
                        v8::Local<v8::Function> handler = m_request_handler.Get(p_isolate);
                        v8::Local<v8::Value> argv[2] = {
                            request->toObject(),
                            response->toObject()
                        };
                        
                        v8::TryCatch try_catch(p_isolate);
                        handler->Call(context, context->Global(), 2, argv);
                        
                        if (try_catch.HasCaught()) {
                            // Send 500 error
                            response->writeHead(500, v8::Local<v8::Object>());
                            response->end("Internal Server Error");
                        }
                    }
                    
                    delete request;
                    delete response;
                }
                
                closesocket(client_socket);
            }).detach();
        }
    }).detach();
#endif
}

void HTTPServer::close(v8::Local<v8::Function> callback) {
    if (!m_listening) return;
    
    m_listening = false;
    
#ifdef _WIN32
    if (m_listen_socket != INVALID_SOCKET) {
        closesocket(m_listen_socket);
        m_listen_socket = INVALID_SOCKET;
    }
#endif
    
    if (!callback.IsEmpty()) {
        m_close_callback.Reset(p_isolate, callback);
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Function> cb = m_close_callback.Get(p_isolate);
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        cb->Call(context, context->Global(), 0, nullptr).ToLocalChecked();
    }
}

void HTTPServer::setRequestHandler(v8::Local<v8::Function> handler) {
    m_request_handler.Reset(p_isolate, handler);
}

// HTTPRequest implementation
HTTPRequest::HTTPRequest(v8::Isolate* p_isolate) : p_isolate(p_isolate) {}

v8::Local<v8::Object> HTTPRequest::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    
    v8::Local<v8::Object> obj = tmpl->NewInstance(context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));
    
    // Set properties
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "method"),
             v8::String::NewFromUtf8(p_isolate, m_method.c_str()).ToLocalChecked()).Check();
    
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "url"),
             v8::String::NewFromUtf8(p_isolate, m_url.c_str()).ToLocalChecked()).Check();
    
    // Headers object
    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    for (const auto& pair : m_headers) {
        headers->Set(context,
                    v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked(),
                    v8::String::NewFromUtf8(p_isolate, pair.second.c_str()).ToLocalChecked()).Check();
    }
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "headers"), headers).Check();
    
    return handle_scope.Escape(obj);
}

void HTTPRequest::addHeader(const std::string& name, const std::string& value) {
    m_headers[name] = value;
}

// HTTPResponse implementation
HTTPResponse::HTTPResponse(v8::Isolate* p_isolate, SOCKET socket)
    : p_isolate(p_isolate), m_socket(socket), m_status_code(200),
      m_headers_sent(false), m_finished(false) {}

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

void HTTPResponse::writeHead(int32_t status_code, v8::Local<v8::Object> headers) {
    if (m_headers_sent) return;
    
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

void HTTPResponse::write(const std::string& data) {
    if (!m_headers_sent) sendHeaders();
    if (m_finished) return;
    
    send(m_socket, data.c_str(), (int32_t)data.length(), 0);
}

void HTTPResponse::end(const std::string& data) {
    if (m_finished) return;
    
    if (!m_headers_sent) sendHeaders();
    
    if (!data.empty()) {
        send(m_socket, data.c_str(), (int32_t)data.length(), 0);
    }
    
    m_finished = true;
}

void HTTPResponse::sendHeaders() {
    if (m_headers_sent) return;
    
    std::ostringstream ss;
    
    // Status line
    ss << "HTTP/1.1 " << m_status_code << " ";
    switch (m_status_code) {
        case 200: ss << "OK"; break;
        case 404: ss << "Not Found"; break;
        case 500: ss << "Internal Server Error"; break;
        default: ss << "Unknown"; break;
    }
    ss << "\r\n";
    
    // Headers
    for (const auto& pair : m_headers) {
        ss << pair.first << ": " << pair.second << "\r\n";
    }
    
    // End of headers
    ss << "\r\n";
    
    std::string headers_str = ss.str();
    send(m_socket, headers_str.c_str(), (int32_t)headers_str.length(), 0);
    
    m_headers_sent = true;
}

// HTTP module interface
v8::Local<v8::ObjectTemplate> HTTP::createTemplate(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    
    // Server methods
    tmpl->Set(p_isolate, "createServer", v8::FunctionTemplate::New(p_isolate, createServer));
    
    // Client methods
    tmpl->Set(p_isolate, "request", v8::FunctionTemplate::New(p_isolate, request));
    tmpl->Set(p_isolate, "get", v8::FunctionTemplate::New(p_isolate, get));
    
    // Constants
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "METHODS"), getMethods);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "STATUS_CODES"), getStatusCodes);
    
    return handle_scope.Escape(tmpl);
}

void HTTP::createServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    // Create server instance
    HTTPServer* server = new HTTPServer(p_isolate);
    
    // Get request handler if provided
    if (args.Length() > 0 && args[0]->IsFunction()) {
        server->setRequestHandler(args[0].As<v8::Function>());
    }
    
    // Create server object
    v8::Local<v8::ObjectTemplate> server_tmpl = v8::ObjectTemplate::New(p_isolate);
    server_tmpl->SetInternalFieldCount(1);
    server_tmpl->Set(p_isolate, "listen", v8::FunctionTemplate::New(p_isolate, serverListen));
    server_tmpl->Set(p_isolate, "close", v8::FunctionTemplate::New(p_isolate, serverClose));
    
    v8::Local<v8::Object> server_obj = server_tmpl->NewInstance(context).ToLocalChecked();
    server_obj->SetInternalField(0, v8::External::New(p_isolate, server));
    
    args.GetReturnValue().Set(server_obj);
}

void HTTP::serverListen(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    HTTPServer* server = unwrapServer(args.This());
    if (!server) return;
    
    // Parse arguments: port, [host], [callback]
    int32_t port = 3000;
    std::string host = "0.0.0.0";
    v8::Local<v8::Function> callback;
    
    if (args.Length() > 0 && args[0]->IsNumber()) {
        port = args[0]->Int32Value(p_isolate->GetCurrentContext()).ToChecked();
    }
    
    if (args.Length() > 1 && args[1]->IsString()) {
        v8::String::Utf8Value host_val(p_isolate, args[1]);
        host = std::string(*host_val, host_val.length());
    }
    
    if (args.Length() > 0 && args[args.Length() - 1]->IsFunction()) {
        callback = args[args.Length() - 1].As<v8::Function>();
    }
    
    server->listen(port, host, callback);
    
    args.GetReturnValue().Set(args.This());
}

void HTTP::serverClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    HTTPServer* server = unwrapServer(args.This());
    if (!server) return;
    
    v8::Local<v8::Function> callback;
    if (args.Length() > 0 && args[0]->IsFunction()) {
        callback = args[0].As<v8::Function>();
    }
    
    server->close(callback);
}

void HTTP::responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    HTTPResponse* response = unwrapResponse(args.This());
    if (!response) return;
    
    int32_t status_code = 200;
    v8::Local<v8::Object> headers;
    
    if (args.Length() > 0 && args[0]->IsNumber()) {
        status_code = args[0]->Int32Value(p_isolate->GetCurrentContext()).ToChecked();
    }
    
    if (args.Length() > 1 && args[1]->IsObject()) {
        headers = args[1].As<v8::Object>();
    }
    
    response->writeHead(status_code, headers);
}

void HTTP::responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    HTTPResponse* response = unwrapResponse(args.This());
    if (!response) return;
    
    if (args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value data(p_isolate, args[0]);
        response->write(std::string(*data, data.length()));
    }
}

void HTTP::responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    HTTPResponse* response = unwrapResponse(args.This());
    if (!response) return;
    
    std::string data;
    if (args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value data_str(p_isolate, args[0]);
        data = std::string(*data_str, data_str.length());
    }
    
    response->end(data);
}

void HTTP::request(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    // TODO: Implement HTTP client
    p_isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(p_isolate, "http.request() not yet implemented")));
}

void HTTP::get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    // TODO: Implement HTTP client
    p_isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(p_isolate, "http.get() not yet implemented")));
}

void HTTP::getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    const char* methods[] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "CONNECT", "TRACE"
    };
    
    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, 9);
    for (int32_t i = 0; i < 9; i++) {
        arr->Set(context, i, v8::String::NewFromUtf8(p_isolate, methods[i]).ToLocalChecked()).Check();
    }
    
    info.GetReturnValue().Set(arr);
}

void HTTP::getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    v8::Local<v8::Object> codes = v8::Object::New(p_isolate);
    
    // Common HTTP status codes
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "200"), 
               v8::String::NewFromUtf8Literal(p_isolate, "OK")).Check();
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "201"), 
               v8::String::NewFromUtf8Literal(p_isolate, "Created")).Check();
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "204"), 
               v8::String::NewFromUtf8Literal(p_isolate, "No Content")).Check();
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "400"), 
               v8::String::NewFromUtf8Literal(p_isolate, "Bad Request")).Check();
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "404"), 
               v8::String::NewFromUtf8Literal(p_isolate, "Not Found")).Check();
    codes->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "500"), 
               v8::String::NewFromUtf8Literal(p_isolate, "Internal Server Error")).Check();
    
    info.GetReturnValue().Set(codes);
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

} // namespace module
} // namespace z8
