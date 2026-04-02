#include "http.hpp"
#include "task_queue.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <memory>
#include <trantor/net/InetAddress.h>
#include <atomic>

#include "../events/events.hpp"
#include "../buffer/buffer.hpp"

namespace z8 {
namespace module {

std::atomic<int32_t> HTTPServer::m_active_servers{0};

// llhttp callbacks (to bridge with HTTPRequest)
static int32_t on_url(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->setUrl(std::string(p_at, length));
    return 0;
}

static int32_t on_header_field(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    if (!p_request->m_current_header_field.empty() && !p_request->m_current_header_value.empty()) {
        p_request->addHeader(p_request->m_current_header_field, p_request->m_current_header_value);
        p_request->m_current_header_value.clear();
    }
    p_request->m_current_header_field = std::string(p_at, length);
    return 0;
}

static int32_t on_header_value(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->m_current_header_value = std::string(p_at, length);
    return 0;
}

static int32_t on_headers_complete(llhttp_t* p_parser) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    if (!p_request->m_current_header_field.empty() && !p_request->m_current_header_value.empty()) {
        p_request->addHeader(p_request->m_current_header_field, p_request->m_current_header_value);
        p_request->m_current_header_field.clear();
        p_request->m_current_header_value.clear();
    }
    const char* p_method_name = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(p_parser)));
    p_request->setMethod(p_method_name);
    p_request->m_http_major = p_parser->http_major;
    p_request->m_http_minor = p_parser->http_minor;
    p_request->pushEvent(HTTPRequest::HttpEventType::HEADERS);
    return 0;
}

static int32_t on_body(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->pushEvent(HTTPRequest::HttpEventType::BODY_CHUNK, std::string(p_at, length));
    return 0;
}

static int32_t on_message_complete(llhttp_t* p_parser) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->m_parsing_complete = true;
    p_request->pushEvent(HTTPRequest::HttpEventType::MESSAGE_COMPLETE);
    return 0;
}

// HTTPServer Implementation
HTTPServer::HTTPServer(v8::Isolate* p_isolate)
    : p_isolate(p_isolate), m_listening(false), m_port(0) {
}

HTTPServer::~HTTPServer() {
    if (m_listening) {
        if (up_tcp_server) up_tcp_server->stop();
        m_active_servers--;
    }
}

void HTTPServer::setRequestHandler(v8::Local<v8::Function> handler) {
    m_request_handler.Reset(p_isolate, handler);
}

void HTTPServer::listen(int32_t port, const std::string& host, v8::Local<v8::Function> callback) {
    if (m_listening) return;

    m_port = port;
    m_host = host;
    
    // Configure Trantor loop and server
    trantor::EventLoop* p_loop = m_loop_thread.getLoop();
    m_loop_thread.run();

    trantor::InetAddress addr(host, (uint16_t)port);
    up_tcp_server = std::make_unique<trantor::TcpServer>(p_loop, addr, "z8_http_server");
    
    // Set thread num for performance (use all cores)
    up_tcp_server->setIoLoopNum(std::thread::hardware_concurrency());

    // Message handler: parse with llhttp, then bridge to Main Event Loop
    up_tcp_server->setRecvMessageCallback([this](const trantor::TcpConnectionPtr& p_conn, trantor::MsgBuffer* p_buffer) {
        // Find or create request parsing state for this connection
        std::shared_ptr<HTTPRequest> sp_request;
        if (!p_conn->hasContext()) {
            sp_request = std::make_shared<HTTPRequest>(p_isolate);
            p_conn->setContext(sp_request);
        } else {
            sp_request = p_conn->getContext<HTTPRequest>();
        }

        // Parse data
        llhttp_errno_t err = sp_request->parse(p_buffer->peek(), p_buffer->readableBytes());
        if (err != HPE_OK) {
            std::cerr << "llhttp parse error: " << llhttp_errno_name(err) << std::endl;
            p_conn->forceClose();
            return;
        }

        // Consume read bytes
        p_buffer->retrieveAll();

        // Process pending events by queueing a task to the Main Thread
        auto events = sp_request->popEvents();
        if (!events.empty()) {
            Task* p_task = new Task();
            p_task->m_runner = [this, sp_request, p_conn, events](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
                v8::HandleScope handle_scope(p_isolate);
                
                v8::Local<v8::Object> req_obj = sp_request->toObject();
                
                v8::Local<v8::Function> emit_fn = req_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocalChecked().As<v8::Function>();

                for (const auto& ev : events) {
                    if (ev.m_type == HTTPRequest::HttpEventType::HEADERS) {
                        if (!m_request_handler.IsEmpty()) {
                            v8::Local<v8::Function> handler = m_request_handler.Get(p_isolate);
                            auto* p_response = new HTTPResponse(p_isolate, p_conn);
                            v8::Local<v8::Object> res_obj = p_response->toObject();

                            v8::Local<v8::Value> argv[2] = { req_obj, res_obj };
                            v8::TryCatch try_catch(p_isolate);
                            
                            (void)handler->Call(context, context->Global(), 2, argv);
                            
                            if (try_catch.HasCaught()) {
                                v8::String::Utf8Value error(p_isolate, try_catch.Exception());
                                std::cerr << "Error in HTTP handler: " << *error << std::endl;
                            }
                        }
                    } else if (ev.m_type == HTTPRequest::HttpEventType::BODY_CHUNK) {
                        v8::Local<v8::Value> chunk = z8::module::Buffer::createBuffer(p_isolate, ev.m_data.length());
                        // Copy data into buffer... wait, createBuffer returns Uint8Array. 
                        // Instead, we will instantiate a new Buffer object from memory
                        v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, ev.m_data.length());
                        std::memcpy(ab->Data(), ev.m_data.data(), ev.m_data.length());
                        // Just use Uint8Array for chunk right now to keep it simple, it's compatible
                        v8::Local<v8::Uint8Array> ui8 = v8::Uint8Array::New(ab, 0, ev.m_data.length());
                        
                        v8::Local<v8::Value> emit_args[2] = {
                            v8::String::NewFromUtf8Literal(p_isolate, "data"),
                            ui8
                        };
                        (void)emit_fn->Call(context, req_obj, 2, emit_args);
                    } else if (ev.m_type == HTTPRequest::HttpEventType::MESSAGE_COMPLETE) {
                        v8::Local<v8::Value> emit_args[1] = {
                            v8::String::NewFromUtf8Literal(p_isolate, "end")
                        };
                        (void)emit_fn->Call(context, req_obj, 1, emit_args);
                        
                        // For Keep-Alive handling
                        if (p_conn->connected()) {
                            p_conn->setContext(std::make_shared<HTTPRequest>(p_isolate));
                        }
                    }
                }
            };
            TaskQueue::getInstance().enqueue(p_task);
        }
    });

    // Cleanup context when connection dies
    up_tcp_server->setConnectionCallback([](const trantor::TcpConnectionPtr& p_conn) {
        if (!p_conn->connected()) {
            p_conn->clearContext();
        }
    });

    up_tcp_server->start();
    m_listening = true;
    m_active_servers++;

    if (!callback.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
        (void)callback->Call(p_context, p_context->Global(), 0, nullptr);
    }
}

void HTTPServer::close(v8::Local<v8::Function> callback) {
    if (!m_listening) return;
    if (up_tcp_server) up_tcp_server->stop();
    m_listening = false;
    m_active_servers--;
    if (!callback.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
        (void)callback->Call(p_context, p_context->Global(), 0, nullptr);
    }
}

// HTTPRequest Implementation
HTTPRequest::HTTPRequest(v8::Isolate* p_isolate) 
    : p_isolate(p_isolate), m_parsing_complete(false) {
    llhttp_settings_init(&m_settings);
    m_settings.on_url = on_url;
    m_settings.on_header_field = on_header_field;
    m_settings.on_header_value = on_header_value;
    m_settings.on_headers_complete = on_headers_complete;
    m_settings.on_body = on_body;
    m_settings.on_message_complete = on_message_complete;
    
    llhttp_init(&m_parser, HTTP_REQUEST, &m_settings);
    m_parser.data = this;
}

HTTPRequest::~HTTPRequest() {
}

llhttp_errno_t HTTPRequest::parse(const char* p_data, size_t length) {
    return llhttp_execute(&m_parser, p_data, length);
}

void HTTPRequest::pushEvent(HttpEventType type, const std::string& data) {
    std::lock_guard<std::mutex> lock(m_events_mutex);
    m_pending_events.push_back({type, data});
}

std::vector<HTTPRequest::HttpEvent> HTTPRequest::popEvents() {
    std::lock_guard<std::mutex> lock(m_events_mutex);
    std::vector<HttpEvent> events = std::move(m_pending_events);
    m_pending_events.clear();
    return events;
}

void HTTPRequest::addHeader(const std::string& name, const std::string& value) {
    m_headers[name] = value;
    m_raw_headers.push_back(name);
    m_raw_headers.push_back(value);
}

void HTTPRequest::setJSObject(v8::Local<v8::Object> obj) {
    m_req_obj.Reset(p_isolate, obj);
}

v8::Local<v8::Object> HTTPRequest::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> obj;
    
    if (m_req_obj.IsEmpty()) {
        // Create EventEmitter instance
        v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
        v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(p_context).ToLocalChecked();
        obj = ee_ctor->NewInstance(p_context).ToLocalChecked();
        m_req_obj.Reset(p_isolate, obj);
    } else {
        obj = m_req_obj.Get(p_isolate);
    }
    
    // Update properties
    std::string http_version = std::to_string(m_http_major) + "." + std::to_string(m_http_minor);
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "httpVersion"),
             v8::String::NewFromUtf8(p_isolate, http_version.c_str()).ToLocalChecked()).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "httpVersionMajor"),
             v8::Integer::New(p_isolate, m_http_major)).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "httpVersionMinor"),
             v8::Integer::New(p_isolate, m_http_minor)).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "complete"),
             v8::Boolean::New(p_isolate, m_parsing_complete)).Check();
             
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "method"),
             v8::String::NewFromUtf8(p_isolate, m_method.c_str()).ToLocalChecked()).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "url"),
             v8::String::NewFromUtf8(p_isolate, m_url.c_str()).ToLocalChecked()).Check();
    
    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    v8::Local<v8::Array> raw_headers = v8::Array::New(p_isolate, m_raw_headers.size());
    
    for (size_t i = 0; i < m_raw_headers.size(); i += 2) {
        std::string name = m_raw_headers[i];
        std::string value = m_raw_headers[i+1];
        
        // Add to rawHeaders array
        raw_headers->Set(p_context, i, v8::String::NewFromUtf8(p_isolate, name.c_str()).ToLocalChecked()).Check();
        raw_headers->Set(p_context, i+1, v8::String::NewFromUtf8(p_isolate, value.c_str()).ToLocalChecked()).Check();
        
        // Lowercase header names for 'headers' object
        std::string lower_name = name;
        for (auto& c : lower_name) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        headers->Set(p_context,
                    v8::String::NewFromUtf8(p_isolate, lower_name.c_str()).ToLocalChecked(),
                    v8::String::NewFromUtf8(p_isolate, value.c_str()).ToLocalChecked()).Check();
    }
    
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "headers"), headers).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "rawHeaders"), raw_headers).Check();
    
    return handle_scope.Escape(obj);
}

// HTTPResponse Implementation
HTTPResponse::HTTPResponse(v8::Isolate* p_isolate, const trantor::TcpConnectionPtr& p_conn)
    : p_isolate(p_isolate), m_conn(p_conn), m_status_code(200),
      m_headers_sent(false), m_finished(false) {}

v8::Local<v8::Object> HTTPResponse::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    
    // Properties
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "statusCode"), 
                      HTTP::responseGetStatusCode, HTTP::responseSetStatusCode);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "statusMessage"), 
                      HTTP::responseGetStatusMessage, HTTP::responseSetStatusMessage);
                      
    tmpl->Set(p_isolate, "writeHead", v8::FunctionTemplate::New(p_isolate, HTTP::responseWriteHead));
    tmpl->Set(p_isolate, "setHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseSetHeader));
    tmpl->Set(p_isolate, "getHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeader));
    tmpl->Set(p_isolate, "hasHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseHasHeader));
    tmpl->Set(p_isolate, "removeHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseRemoveHeader));
    tmpl->Set(p_isolate, "getHeaderNames", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeaderNames));
    tmpl->Set(p_isolate, "getHeaders", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeaders));
    tmpl->Set(p_isolate, "write", v8::FunctionTemplate::New(p_isolate, HTTP::responseWrite));
    tmpl->Set(p_isolate, "end", v8::FunctionTemplate::New(p_isolate, HTTP::responseEnd));
    v8::Local<v8::Object> obj = tmpl->NewInstance(p_context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));
    return handle_scope.Escape(obj);
}

void HTTPResponse::writeHead(int32_t status_code, v8::Local<v8::Object> headers) {
    writeHead(status_code, "", headers);
}

void HTTPResponse::writeHead(int32_t status_code, const std::string& status_message, v8::Local<v8::Object> headers) {
    if (m_headers_sent) return;
    m_status_code = status_code;
    if (!status_message.empty()) {
        m_status_message = status_message;
    }
    if (!headers.IsEmpty()) {
        v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
        v8::Local<v8::Array> props = headers->GetPropertyNames(p_context).ToLocalChecked();
        for (uint32_t i = 0; i < props->Length(); i++) {
            v8::Local<v8::Value> key = props->Get(p_context, i).ToLocalChecked();
            v8::Local<v8::Value> val = headers->Get(p_context, key).ToLocalChecked();
            v8::String::Utf8Value key_str(p_isolate, key);
            v8::String::Utf8Value val_str(p_isolate, val);
            m_headers[*key_str] = *val_str;
        }
    }
    sendHeaders();
}

void HTTPResponse::setHeader(const std::string& name, const std::string& value) {
    if (m_headers_sent) return;
    m_headers[name] = value;
}

bool HTTPResponse::hasHeader(const std::string& name) {
    return m_headers.find(name) != m_headers.end();
}

std::string HTTPResponse::getHeader(const std::string& name) {
    auto it = m_headers.find(name);
    if (it != m_headers.end()) {
        return it->second;
    }
    return "";
}

void HTTPResponse::removeHeader(const std::string& name) {
    if (m_headers_sent) return;
    m_headers.erase(name);
}

v8::Local<v8::Array> HTTPResponse::getHeaderNames() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Array> names = v8::Array::New(p_isolate, m_headers.size());
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    int32_t i = 0;
    for (const auto& pair : m_headers) {
        names->Set(p_context, i++, v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked()).Check();
    }
    return handle_scope.Escape(names);
}

v8::Local<v8::Object> HTTPResponse::getHeaders() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Object> hdrs = v8::Object::New(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    for (const auto& pair : m_headers) {
        hdrs->Set(p_context, 
                 v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked(),
                 v8::String::NewFromUtf8(p_isolate, pair.second.c_str()).ToLocalChecked()).Check();
    }
    return handle_scope.Escape(hdrs);
}

void HTTPResponse::write(const std::string& data) {
    if (!m_headers_sent) sendHeaders();
    if (m_finished) return;
    m_conn->send(data);
}

void HTTPResponse::end(const std::string& data) {
    if (m_finished) return;
    if (!m_headers_sent) sendHeaders();
    if (!data.empty()) m_conn->send(data);
    m_finished = true;
}

void HTTPResponse::sendHeaders() {
    if (m_headers_sent) return;
    std::ostringstream ss;
    std::string msg = m_status_message.empty() ? "OK" : m_status_message;
    if (m_status_message.empty()) {
        if (m_status_code == 404) msg = "Not Found";
        else if (m_status_code == 500) msg = "Internal Server Error";
        else if (m_status_code == 201) msg = "Created";
        else if (m_status_code == 204) msg = "No Content";
        else if (m_status_code == 400) msg = "Bad Request";
        else if (m_status_code == 401) msg = "Unauthorized";
        else if (m_status_code == 403) msg = "Forbidden";
    }
    ss << "HTTP/1.1 " << m_status_code << " " << msg << "\r\n";
    for (const auto& pair : m_headers) {
        ss << pair.first << ": " << pair.second << "\r\n";
    }
    ss << "\r\n";
    m_conn->send(ss.str());
    m_headers_sent = true;
}

// HTTP Module Template
v8::Local<v8::ObjectTemplate> HTTP::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->Set(p_isolate, "createServer", v8::FunctionTemplate::New(p_isolate, createServer));
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "METHODS"), getMethods);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "STATUS_CODES"), getStatusCodes);
    return tmpl;
}

void HTTP::createServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    auto* p_server = new HTTPServer(p_isolate);
    if (args.Length() > 0 && args[0]->IsFunction()) {
        p_server->setRequestHandler(args[0].As<v8::Function>());
    }
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    tmpl->Set(p_isolate, "listen", v8::FunctionTemplate::New(p_isolate, serverListen));
    tmpl->Set(p_isolate, "close", v8::FunctionTemplate::New(p_isolate, serverClose));
    v8::Local<v8::Object> obj = tmpl->NewInstance(p_isolate->GetCurrentContext()).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, p_server));
    args.GetReturnValue().Set(obj);
}

void HTTP::serverListen(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_server = unwrapServer(args.This());
    if (!p_server) return;
    int32_t port = 3000;
    std::string host = "0.0.0.0";
    v8::Local<v8::Function> callback;
    if (args.Length() > 0 && args[0]->IsNumber()) port = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
    if (args.Length() > 1 && args[1]->IsString()) {
        v8::String::Utf8Value host_val(args.GetIsolate(), args[1]);
        host = std::string(*host_val, host_val.length());
    }
    if (args.Length() > 0 && args[args.Length()-1]->IsFunction()) callback = args[args.Length()-1].As<v8::Function>();
    p_server->listen(port, host, callback);
}

void HTTP::serverClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_server = unwrapServer(args.This());
    if (p_server) {
        v8::Local<v8::Function> callback;
        if (args.Length() > 0 && args[0]->IsFunction()) callback = args[0].As<v8::Function>();
        p_server->close(callback);
    }
}

void HTTP::responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        int32_t status = 200;
        v8::Local<v8::Object> headers;
        if (args.Length() > 0 && args[0]->IsNumber()) status = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
        if (args.Length() > 1 && args[1]->IsObject()) headers = args[1].As<v8::Object>();
        p_response->writeHead(status, headers);
    }
}

void HTTP::responseSetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 2 && args[0]->IsString() && args[1]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        v8::String::Utf8Value value(p_isolate, args[1]);
        p_response->setHeader(std::string(*name, name.length()), std::string(*value, value.length()));
    }
}

void HTTP::responseGetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 1 && args[0]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        std::string val = p_response->getHeader(std::string(*name, name.length()));
        if (!val.empty()) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked());
        } else {
            args.GetReturnValue().SetNull();
        }
    }
}

void HTTP::responseHasHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 1 && args[0]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, p_response->hasHeader(std::string(*name, name.length()))));
    }
}

void HTTP::responseRemoveHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 1 && args[0]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        p_response->removeHeader(std::string(*name, name.length()));
    }
}

void HTTP::responseGetHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        args.GetReturnValue().Set(p_response->getHeaderNames());
    }
}

void HTTP::responseGetHeaders(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        args.GetReturnValue().Set(p_response->getHeaders());
    }
}

void HTTP::responseWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value val(args.GetIsolate(), args[0]);
        p_response->write(std::string(*val, val.length()));
    }
}

void HTTP::responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        std::string data;
        if (args.Length() > 0 && args[0]->IsString()) {
            v8::String::Utf8Value val(args.GetIsolate(), args[0]);
            data = std::string(*val, val.length());
        }
        p_response->end(data);
    }
}

void HTTP::getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    const char* methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};
    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, 7);
    for (int32_t i = 0; i < 7; i++) arr->Set(p_isolate->GetCurrentContext(), i, v8::String::NewFromUtf8(p_isolate, methods[i]).ToLocalChecked()).Check();
    info.GetReturnValue().Set(arr);
}

void HTTP::getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    v8::Local<v8::Object> codes = v8::Object::New(p_isolate);
    codes->Set(p_isolate->GetCurrentContext(), v8::String::NewFromUtf8Literal(p_isolate, "200"), v8::String::NewFromUtf8Literal(p_isolate, "OK")).Check();
    info.GetReturnValue().Set(codes);
}

void HTTP::responseGetStatusCode(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response) {
        info.GetReturnValue().Set(v8::Integer::New(info.GetIsolate(), p_response->getStatusCode()));
    }
}

void HTTP::responseSetStatusCode(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response && value->IsNumber()) {
        p_response->setStatusCode(value->Int32Value(info.GetIsolate()->GetCurrentContext()).ToChecked());
    }
}

void HTTP::responseGetStatusMessage(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response) {
        std::string msg = p_response->getStatusMessage();
        if (!msg.empty()) {
            info.GetReturnValue().Set(v8::String::NewFromUtf8(info.GetIsolate(), msg.c_str()).ToLocalChecked());
        } else {
            info.GetReturnValue().SetEmptyString();
        }
    }
}

void HTTP::responseSetStatusMessage(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response && value->IsString()) {
        v8::String::Utf8Value msg(info.GetIsolate(), value);
        p_response->setStatusMessage(std::string(*msg, msg.length()));
    }
}

HTTPServer* HTTP::unwrapServer(v8::Local<v8::Object> obj) {
    if (obj->InternalFieldCount() > 0) return static_cast<HTTPServer*>(obj->GetInternalField(0).As<v8::External>()->Value());
    return nullptr;
}

HTTPResponse* HTTP::unwrapResponse(v8::Local<v8::Object> obj) {
    if (obj->InternalFieldCount() > 0) return static_cast<HTTPResponse*>(obj->GetInternalField(0).As<v8::External>()->Value());
    return nullptr;
}

} // namespace module
} // namespace z8
