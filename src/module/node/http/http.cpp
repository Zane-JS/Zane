#include "http.hpp"
#include "task_queue.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <memory>
#include <trantor/net/InetAddress.h>
#include <atomic>

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
    return 0;
}

static int32_t on_body(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->appendBody(p_at, length);
    return 0;
}

static int32_t on_message_complete(llhttp_t* p_parser) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->m_parsing_complete = true;
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

        // If message is complete, push a task to the Main Event Loop
        if (sp_request->m_parsing_complete) {
            // Important: Mark parsing as in-progress to prevent multiple tasks for the same request
            // while it's still in the queue. For HTTP/1.1 pipelining, we handle it sequentially.
            
            p_conn->clearContext(); // Connection will wait for the next request setup

            // Create a Task for the Main Event Loop
            Task* p_task = new Task();
            p_task->m_runner = [this, sp_request, p_conn](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
                v8::HandleScope handle_scope(p_isolate);
                
                if (!m_request_handler.IsEmpty()) {
                    v8::Local<v8::Function> handler = m_request_handler.Get(p_isolate);
                    
                    // Create JS objects on the main thread
                    v8::Local<v8::Object> req_obj = sp_request->toObject();
                    
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

                // If connection is still alive, set context for next request (Keep-Alive)
                if (p_conn->connected()) {
                    p_conn->setContext(std::make_shared<HTTPRequest>(p_isolate));
                }
            };
            
            // Push to Main Event Loop
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

void HTTPRequest::addHeader(const std::string& name, const std::string& value) {
    m_headers[name] = value;
}

void HTTPRequest::appendBody(const char* p_data, size_t length) {
    m_body.append(p_data, length);
}

v8::Local<v8::Object> HTTPRequest::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> obj = v8::Object::New(p_isolate);
    
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "method"),
             v8::String::NewFromUtf8(p_isolate, m_method.c_str()).ToLocalChecked()).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "url"),
             v8::String::NewFromUtf8(p_isolate, m_url.c_str()).ToLocalChecked()).Check();
    
    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    for (const auto& pair : m_headers) {
        headers->Set(p_context,
                    v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked(),
                    v8::String::NewFromUtf8(p_isolate, pair.second.c_str()).ToLocalChecked()).Check();
    }
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "headers"), headers).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "body"),
             v8::String::NewFromUtf8(p_isolate, m_body.c_str(), v8::NewStringType::kNormal, m_body.length()).ToLocalChecked()).Check();
    
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
    tmpl->Set(p_isolate, "writeHead", v8::FunctionTemplate::New(p_isolate, HTTP::responseWriteHead));
    tmpl->Set(p_isolate, "write", v8::FunctionTemplate::New(p_isolate, HTTP::responseWrite));
    tmpl->Set(p_isolate, "end", v8::FunctionTemplate::New(p_isolate, HTTP::responseEnd));
    v8::Local<v8::Object> obj = tmpl->NewInstance(p_context).ToLocalChecked();
    obj->SetInternalField(0, v8::External::New(p_isolate, this));
    return handle_scope.Escape(obj);
}

void HTTPResponse::writeHead(int32_t status_code, v8::Local<v8::Object> headers) {
    if (m_headers_sent) return;
    m_status_code = status_code;
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
    ss << "HTTP/1.1 " << m_status_code << " OK\r\n";
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
