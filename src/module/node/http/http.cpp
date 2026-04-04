#include "http.hpp"
#include "task_queue.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <memory>
#include <cstring>
#include <ctime>
#include <trantor/net/InetAddress.h>
#include <trantor/net/TcpClient.h>
#include <atomic>

#include "../events/events.hpp"
#include "../buffer/buffer.hpp"

namespace z8 {
namespace module {

std::atomic<int32_t> HTTPServer::m_active_servers{0};

namespace {

template <typename T>
T* unwrapExternalPointer(v8::Local<v8::Object> obj) {
    if (obj.IsEmpty() || obj->InternalFieldCount() < 1) {
        return nullptr;
    }

    return static_cast<T*>(obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
}

template <typename T>
void enqueueDeferredDelete(T* p_object) {
    Task* p_task = new Task();
    p_task->m_runner = [p_object](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
        delete p_object;
    };
    TaskQueue::getInstance().enqueue(p_task);
}

std::string normalizeHeaderName(const std::string& name) {
    std::string normalized = name;
    for (char& c : normalized) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return normalized;
}

bool shouldDiscardDuplicateHeader(const std::string& name) {
    return name == "age" ||
           name == "authorization" ||
           name == "content-length" ||
           name == "content-type" ||
           name == "etag" ||
           name == "expires" ||
           name == "from" ||
           name == "host" ||
           name == "if-modified-since" ||
           name == "if-unmodified-since" ||
           name == "last-modified" ||
           name == "location" ||
           name == "max-forwards" ||
           name == "proxy-authorization" ||
           name == "referer" ||
           name == "retry-after" ||
           name == "server" ||
           name == "user-agent";
}

std::string getDefaultStatusMessage(int32_t status_code) {
    switch (status_code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default:
        return "";
    }
}

bool tryParsePort(const std::string& value, int32_t* p_port) {
    if (!p_port || value.empty()) {
        return false;
    }

    try {
        size_t parsed = 0;
        int32_t port = std::stoi(value, &parsed);
        if (parsed != value.size() || port < 0 || port > 65535) {
            return false;
        }
        *p_port = port;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool extractHeaderValues(v8::Isolate* p_isolate,
                         v8::Local<v8::Value> value,
                         std::vector<std::string>* p_values) {
    if (!p_values) {
        return false;
    }

    p_values->clear();
    if (value->IsArray()) {
        v8::Local<v8::Array> array = value.As<v8::Array>();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        for (uint32_t i = 0; i < array->Length(); i++) {
            v8::Local<v8::Value> entry;
            if (!array->Get(context, i).ToLocal(&entry) || !entry->IsString()) {
                return false;
            }

            v8::String::Utf8Value entry_str(p_isolate, entry);
            p_values->push_back(std::string(*entry_str, entry_str.length()));
        }
        return true;
    }

    if (value->IsString()) {
        v8::String::Utf8Value utf8_value(p_isolate, value);
        p_values->push_back(std::string(*utf8_value, utf8_value.length()));
        return true;
    }

    return false;
}

bool readBinaryValue(v8::Isolate* p_isolate, v8::Local<v8::Value> value, std::string* p_output) {
    if (value->IsString()) {
        v8::String::Utf8Value utf8_value(p_isolate, value);
        p_output->assign(*utf8_value, utf8_value.length());
        return true;
    }

    if (value->IsUint8Array()) {
        v8::Local<v8::Uint8Array> ui8 = value.As<v8::Uint8Array>();
        const uint8_t* p_data = static_cast<const uint8_t*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
        p_output->assign(reinterpret_cast<const char*>(p_data), ui8->ByteLength());
        return true;
    }

    return false;
}

v8::Local<v8::Value> createBufferValue(v8::Isolate* p_isolate, const std::string& data) {
    v8::Local<v8::Uint8Array> buffer = z8::module::Buffer::createBuffer(p_isolate, data.size());
    if (!data.empty()) {
        std::memcpy(buffer->Buffer()->GetBackingStore()->Data(), data.data(), data.size());
    }
    return buffer;
}

std::string formatHttpDate() {
    std::time_t now = std::time(nullptr);
    std::tm gmt_now{};
#ifdef _WIN32
    gmtime_s(&gmt_now, &now);
#else
    gmtime_r(&now, &gmt_now);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &gmt_now);
    return std::string(buffer);
}

void copyObjectProperties(v8::Isolate* p_isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Object> source,
                          v8::Local<v8::Object> target) {
    v8::Local<v8::Array> prop_names = source->GetPropertyNames(context).ToLocalChecked();
    for (uint32_t i = 0; i < prop_names->Length(); i++) {
        v8::Local<v8::Value> key = prop_names->Get(context, i).ToLocalChecked();
        target->Set(context, key, source->Get(context, key).ToLocalChecked()).Check();
    }
}

void weakDeleteResponse(const v8::WeakCallbackInfo<HTTPResponse>& info) {
    HTTPResponse* p_response = info.GetParameter();
    if (!p_response) {
        return;
    }

    p_response->markGcPending();
    p_response->release();
}

void weakDeleteServer(const v8::WeakCallbackInfo<HTTPServer>& info) {
    HTTPServer* p_server = info.GetParameter();
    if (!p_server) {
        return;
    }

    p_server->markGcPending();
    p_server->markDisposed();
    p_server->release();
}

void weakDeleteClientRequest(const v8::WeakCallbackInfo<HTTPClientRequest>& info) {
    HTTPClientRequest* p_request = info.GetParameter();
    if (!p_request) {
        return;
    }

    p_request->markGcPending();
    p_request->release();
}

} // namespace

// llhttp callbacks (to bridge with HTTPRequest)
static int32_t on_url(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->appendUrl(p_at, length);
    return 0;
}

static int32_t on_header_field(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->appendHeaderField(p_at, length);
    return 0;
}

static int32_t on_header_value(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->appendHeaderValue(p_at, length);
    return 0;
}

static int32_t on_headers_complete(llhttp_t* p_parser) {
    auto* p_request = static_cast<HTTPRequest*>(p_parser->data);
    p_request->finishPendingHeader();
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
    : p_isolate(p_isolate),
      m_listening(false),
      m_disposed(false),
      m_gc_pending(false),
      m_ref_count(1),
      m_delete_scheduled(false),
      m_port(0) {
}

HTTPServer::~HTTPServer() {
    if (m_listening) {
        if (up_tcp_server) up_tcp_server->stop();
        m_active_servers--;
    }
    up_tcp_server.reset();
    m_server_obj.Reset();
    m_request_handler.Reset();
}

void HTTPServer::markDisposed() {
    m_disposed = true;
}

void HTTPServer::retain() {
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void HTTPServer::release() {
    int32_t ref_count = m_ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref_count == 0 && m_gc_pending.load(std::memory_order_acquire)) {
        scheduleDelete();
    }
}

void HTTPServer::setRequestHandler(v8::Local<v8::Function> handler) {
    m_request_handler.Reset(p_isolate, handler);
}

void HTTPServer::setJSObject(v8::Local<v8::Object> obj) {
    m_server_obj.Reset(p_isolate, obj);
    m_server_obj.SetWeak(this, weakDeleteServer, v8::WeakCallbackType::kParameter);
}

void HTTPServer::scheduleDelete() {
    bool expected = false;
    if (!m_delete_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    enqueueDeferredDelete(this);
}

void HTTPServer::listen(int32_t port, const std::string& host, v8::Local<v8::Function> callback) {
    if (m_listening) return;

    m_port = port;
    m_host = host;
    retain();
    
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
            sp_request->setConnection(p_conn);
            p_conn->setContext(sp_request);
        } else {
            sp_request = p_conn->getContext<HTTPRequest>();
        }

        // Parse data
        llhttp_errno_t err = sp_request->parse(p_buffer->peek(), p_buffer->readableBytes());
        if (err != HPE_OK) {
            std::cerr << "llhttp parse error: " << llhttp_errno_name(err) << std::endl;
            if (!m_server_obj.IsEmpty()) {
                Task* p_task = new Task();
                std::string error_message = llhttp_errno_name(err);
                retain();
                p_task->m_runner = [this, error_message](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
                    if (!m_server_obj.IsEmpty()) {
                        v8::HandleScope handle_scope(p_isolate);
                        v8::Local<v8::Object> server_obj = m_server_obj.Get(p_isolate);
                        v8::Local<v8::Value> emit_val;
                        if (server_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                            v8::Local<v8::Value> emit_args[2] = {
                                v8::String::NewFromUtf8Literal(p_isolate, "clientError"),
                                v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, error_message.c_str()).ToLocalChecked())
                            };
                            (void)emit_val.As<v8::Function>()->Call(context, server_obj, 2, emit_args);
                        }
                    }
                    release();
                };
                TaskQueue::getInstance().enqueue(p_task);
            }
            p_conn->forceClose();
            return;
        }

        // Consume read bytes
        p_buffer->retrieveAll();

        // Process pending events by queueing a task to the Main Thread
        auto events = sp_request->popEvents();
        if (!events.empty()) {
            retain();
            Task* p_task = new Task();
            p_task->m_runner = [this, sp_request, p_conn, events](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
                v8::HandleScope handle_scope(p_isolate);
                
                v8::Local<v8::Object> req_obj = sp_request->toObject();
                
                v8::Local<v8::Function> emit_fn = req_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocalChecked().As<v8::Function>();

                for (const auto& ev : events) {
                    if (ev.m_type == HTTPRequest::HttpEventType::HEADERS) {
                        if (!m_server_obj.IsEmpty()) {
                            v8::Local<v8::Object> server_obj = m_server_obj.Get(p_isolate);
                            v8::Local<v8::Value> emit_val;
                            if (server_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                                auto* p_response = new HTTPResponse(p_isolate, p_conn);
                                v8::Local<v8::Object> res_obj = p_response->toObject();
                                v8::Local<v8::Value> emit_args[3] = {
                                    v8::String::NewFromUtf8Literal(p_isolate, "request"),
                                    req_obj,
                                    res_obj
                                };
                                (void)emit_val.As<v8::Function>()->Call(context, server_obj, 3, emit_args);
                                if (!m_request_handler.IsEmpty()) {
                                    v8::Local<v8::Function> handler = m_request_handler.Get(p_isolate);
                                    v8::Local<v8::Value> argv[2] = { req_obj, res_obj };
                                    v8::TryCatch try_catch(p_isolate);
                                    (void)handler->Call(context, context->Global(), 2, argv);
                                    if (try_catch.HasCaught()) {
                                        v8::String::Utf8Value error(p_isolate, try_catch.Exception());
                                        std::cerr << "Error in HTTP handler: " << *error << std::endl;
                                    }
                                }
                                continue;
                            }
                        }

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
                        v8::Local<v8::Value> emit_args[2] = {
                            v8::String::NewFromUtf8Literal(p_isolate, "data"),
                            createBufferValue(p_isolate, ev.m_data)
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
                release();
            };
            TaskQueue::getInstance().enqueue(p_task);
        }
    });

    // Cleanup context when connection dies
    up_tcp_server->setConnectionCallback([this](const trantor::TcpConnectionPtr& p_conn) {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        if (p_conn->connected()) {
            m_connections.insert(p_conn);
        } else {
            if (p_conn->hasContext()) {
                std::shared_ptr<HTTPRequest> sp_request = p_conn->getContext<HTTPRequest>();
                if (sp_request) {
                    sp_request->markDestroyed();
                }
            }
            m_connections.erase(p_conn);
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

void HTTPServer::closeAllConnections() {
    std::vector<trantor::TcpConnectionPtr> connections;
    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        connections.assign(m_connections.begin(), m_connections.end());
    }

    for (const trantor::TcpConnectionPtr& p_conn : connections) {
        if (p_conn) {
            p_conn->forceClose();
        }
    }
}

void HTTPServer::close(v8::Local<v8::Function> callback) {
    if (!m_listening) return;
    if (up_tcp_server) up_tcp_server->stop();
    m_listening = false;
    m_active_servers--;
    if (!m_server_obj.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> server_obj = m_server_obj.Get(p_isolate);
        v8::Local<v8::Value> emit_val;
        if (server_obj->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> emit_args[1] = {
                v8::String::NewFromUtf8Literal(p_isolate, "close")
            };
            (void)emit_val.As<v8::Function>()->Call(p_context, server_obj, 1, emit_args);
        }
    }
    if (!callback.IsEmpty()) {
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
        (void)callback->Call(p_context, p_context->Global(), 0, nullptr);
    }
    release();
}

// HTTPRequest Implementation
HTTPRequest::HTTPRequest(v8::Isolate* p_isolate) 
    : p_isolate(p_isolate),
      m_parsing_complete(false),
      m_http_major(1),
      m_http_minor(1),
      m_destroyed(false),
      m_last_header_state(HeaderParseState::NONE) {
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

void HTTPRequest::appendUrl(const char* p_data, size_t length) {
    m_url.append(p_data, length);
}

void HTTPRequest::appendHeaderField(const char* p_data, size_t length) {
    if (m_last_header_state == HeaderParseState::VALUE) {
        finishPendingHeader();
    }
    m_current_header_field.append(p_data, length);
    m_last_header_state = HeaderParseState::FIELD;
}

void HTTPRequest::appendHeaderValue(const char* p_data, size_t length) {
    m_current_header_value.append(p_data, length);
    m_last_header_state = HeaderParseState::VALUE;
}

void HTTPRequest::finishPendingHeader() {
    if (!m_current_header_field.empty()) {
        addHeader(m_current_header_field, m_current_header_value);
    }
    m_current_header_field.clear();
    m_current_header_value.clear();
    m_last_header_state = HeaderParseState::NONE;
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
    m_raw_headers.push_back(name);
    m_raw_headers.push_back(value);
    m_header_values[normalizeHeaderName(name)].push_back(value);
}

void HTTPRequest::setConnection(const trantor::TcpConnectionPtr& conn) {
    m_conn = conn;
}

void HTTPRequest::markDestroyed() {
    m_destroyed = true;
}

void HTTPRequest::destroy() {
    if (m_destroyed) {
        return;
    }

    m_destroyed = true;
    if (m_conn) {
        m_conn->forceClose();
    }
}

void HTTPRequest::setJSObject(v8::Local<v8::Object> obj) {
    m_req_obj.Reset(p_isolate, obj);
}

v8::Local<v8::Object> HTTPRequest::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> obj;
    
    if (m_req_obj.IsEmpty()) {
        v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
        tmpl->SetInternalFieldCount(1);
        tmpl->Set(p_isolate, "destroy", v8::FunctionTemplate::New(p_isolate, HTTP::requestDestroy));
        tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "destroyed"),
                                    HTTP::requestGetDestroyed,
                                    nullptr);
        obj = tmpl->NewInstance(p_context).ToLocalChecked();
        obj->SetAlignedPointerInInternalField(0, this, v8::kEmbedderDataTypeTagDefault);
        v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
        v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(p_context).ToLocalChecked();
        v8::Local<v8::Object> ee_obj = ee_ctor->NewInstance(p_context).ToLocalChecked();
        copyObjectProperties(p_isolate, p_context, ee_obj, obj);
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
    v8::Local<v8::Object> headers_distinct = v8::Object::New(p_isolate);
    v8::Local<v8::Array> raw_headers = v8::Array::New(p_isolate, m_raw_headers.size());
    
    for (size_t i = 0; i < m_raw_headers.size(); i += 2) {
        std::string name = m_raw_headers[i];
        std::string value = m_raw_headers[i+1];
        
        // Add to rawHeaders array
        raw_headers->Set(p_context, i, v8::String::NewFromUtf8(p_isolate, name.c_str()).ToLocalChecked()).Check();
        raw_headers->Set(p_context, i+1, v8::String::NewFromUtf8(p_isolate, value.c_str()).ToLocalChecked()).Check();
        
    }

    for (const auto& pair : m_header_values) {
        const std::string& name = pair.first;
        const std::vector<std::string>& values = pair.second;
        v8::Local<v8::Array> distinct_values = v8::Array::New(p_isolate, values.size());

        for (uint32_t i = 0; i < values.size(); i++) {
            distinct_values->Set(
                p_context,
                i,
                v8::String::NewFromUtf8(p_isolate, values[i].c_str()).ToLocalChecked()).Check();
        }

        headers_distinct->Set(
            p_context,
            v8::String::NewFromUtf8(p_isolate, name.c_str()).ToLocalChecked(),
            distinct_values).Check();

        if (values.empty()) {
            continue;
        }

        v8::Local<v8::Value> header_value;
        if (name == "set-cookie") {
            header_value = distinct_values;
        } else {
            std::ostringstream value_builder;
            if (shouldDiscardDuplicateHeader(name)) {
                value_builder << values.front();
            } else {
                const char* p_separator = (name == "cookie") ? "; " : ", ";
                for (size_t i = 0; i < values.size(); i++) {
                    if (i > 0) {
                        value_builder << p_separator;
                    }
                    value_builder << values[i];
                }
            }
            header_value = v8::String::NewFromUtf8(p_isolate, value_builder.str().c_str()).ToLocalChecked();
        }

        headers->Set(
            p_context,
            v8::String::NewFromUtf8(p_isolate, name.c_str()).ToLocalChecked(),
            header_value).Check();
    }
    
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "headers"), headers).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "headersDistinct"), headers_distinct).Check();
    obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "rawHeaders"), raw_headers).Check();
    
    return handle_scope.Escape(obj);
}

// HTTPResponse Implementation
HTTPResponse::HTTPResponse(v8::Isolate* p_isolate, const trantor::TcpConnectionPtr& p_conn)
    : p_isolate(p_isolate), m_conn(p_conn), m_status_code(200),
      m_headers_sent(false), m_finished(false), m_use_chunked_encoding(false),
      m_send_date(true), m_gc_pending(false), m_ref_count(1), m_delete_scheduled(false) {}

HTTPResponse::~HTTPResponse() {
    m_res_obj.Reset();
}

void HTTPResponse::retain() {
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void HTTPResponse::release() {
    int32_t ref_count = m_ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref_count == 0 && m_gc_pending.load(std::memory_order_acquire)) {
        scheduleDelete();
    }
}

v8::Local<v8::Object> HTTPResponse::toObject() {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (!m_res_obj.IsEmpty()) {
        return handle_scope.Escape(m_res_obj.Get(p_isolate));
    }

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "statusCode"), 
                                HTTP::responseGetStatusCode, HTTP::responseSetStatusCode);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "statusMessage"), 
                                HTTP::responseGetStatusMessage, HTTP::responseSetStatusMessage);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "headersSent"),
                                HTTP::responseGetHeadersSent,
                                nullptr);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "finished"),
                                HTTP::responseGetFinished,
                                nullptr);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableEnded"),
                                HTTP::responseGetFinished,
                                nullptr);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableFinished"),
                                HTTP::responseGetFinished,
                                nullptr);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "sendDate"),
                                HTTP::responseGetSendDate,
                                HTTP::responseSetSendDate);
                       
    tmpl->Set(p_isolate, "writeHead", v8::FunctionTemplate::New(p_isolate, HTTP::responseWriteHead));
    tmpl->Set(p_isolate, "setHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseSetHeader));
    tmpl->Set(p_isolate, "getHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeader));
    tmpl->Set(p_isolate, "hasHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseHasHeader));
    tmpl->Set(p_isolate, "removeHeader", v8::FunctionTemplate::New(p_isolate, HTTP::responseRemoveHeader));
    tmpl->Set(p_isolate, "getHeaderNames", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeaderNames));
    tmpl->Set(p_isolate, "getHeaders", v8::FunctionTemplate::New(p_isolate, HTTP::responseGetHeaders));
    tmpl->Set(p_isolate, "write", v8::FunctionTemplate::New(p_isolate, HTTP::responseWrite));
    tmpl->Set(p_isolate, "end", v8::FunctionTemplate::New(p_isolate, HTTP::responseEnd));
    tmpl->Set(p_isolate, "flushHeaders", v8::FunctionTemplate::New(p_isolate, HTTP::responseFlushHeaders));

    v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
    v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(p_context).ToLocalChecked();
    v8::Local<v8::Object> ee_obj = ee_ctor->NewInstance(p_context).ToLocalChecked();

    v8::Local<v8::Object> res_obj = tmpl->NewInstance(p_context).ToLocalChecked();
    res_obj->SetAlignedPointerInInternalField(0, this, v8::kEmbedderDataTypeTagDefault);
    copyObjectProperties(p_isolate, p_context, ee_obj, res_obj);
    if (m_conn && m_conn->hasContext()) {
        std::shared_ptr<HTTPRequest> sp_request = m_conn->getContext<HTTPRequest>();
        if (sp_request) {
            res_obj->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "req"), sp_request->toObject()).Check();
        }
    }

    m_res_obj.Reset(p_isolate, res_obj);
    m_res_obj.SetWeak(this, weakDeleteResponse, v8::WeakCallbackType::kParameter);
    return handle_scope.Escape(res_obj);
}

void HTTPResponse::writeHead(int32_t status_code, v8::Local<v8::Object> headers) {
    writeHead(status_code, "", headers);
}

void HTTPResponse::writeHead(int32_t status_code, const std::string& status_message, v8::Local<v8::Object> headers) {
    if (m_headers_sent) {
        return;
    }
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
            std::vector<std::string> values;
            if (extractHeaderValues(p_isolate, val, &values)) {
                m_headers[normalizeHeaderName(std::string(*key_str, key_str.length()))] = values;
            }
        }
    }

    if (!hasHeader("content-length") && !hasHeader("transfer-encoding")) {
        m_headers["transfer-encoding"] = {"chunked"};
        m_use_chunked_encoding = true;
    } else if (getFirstHeaderValue("transfer-encoding") == "chunked") {
        m_use_chunked_encoding = true;
    }

    sendHeaders();
}

void HTTPResponse::setHeader(const std::string& name, const std::string& value) {
    setHeader(name, std::vector<std::string>{value});
}

void HTTPResponse::setHeader(const std::string& name, const std::vector<std::string>& values) {
    if (m_headers_sent) {
        return;
    }
    m_headers[normalizeHeaderName(name)] = values;
}

bool HTTPResponse::hasHeader(const std::string& name) {
    return m_headers.find(normalizeHeaderName(name)) != m_headers.end();
}

const std::vector<std::string>* HTTPResponse::getHeader(const std::string& name) const {
    auto it = m_headers.find(normalizeHeaderName(name));
    if (it != m_headers.end()) {
        return &it->second;
    }
    return nullptr;
}

void HTTPResponse::removeHeader(const std::string& name) {
    if (m_headers_sent) {
        return;
    }
    m_headers.erase(normalizeHeaderName(name));
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
        v8::Local<v8::Value> header_value;
        if (pair.second.size() <= 1) {
            std::string value = pair.second.empty() ? "" : pair.second.front();
            header_value = v8::String::NewFromUtf8(p_isolate, value.c_str()).ToLocalChecked();
        } else {
            v8::Local<v8::Array> values = v8::Array::New(p_isolate, static_cast<int32_t>(pair.second.size()));
            for (uint32_t i = 0; i < pair.second.size(); i++) {
                values->Set(p_context,
                            i,
                            v8::String::NewFromUtf8(p_isolate, pair.second[i].c_str()).ToLocalChecked()).Check();
            }
            header_value = values;
        }

        hdrs->Set(p_context,
                  v8::String::NewFromUtf8(p_isolate, pair.first.c_str()).ToLocalChecked(),
                  header_value).Check();
    }
    return handle_scope.Escape(hdrs);
}

void HTTPResponse::write(const std::string& data) {
    if (m_finished || data.empty()) {
        return;
    }

    if (!m_headers_sent) {
        if (!hasHeader("content-length") && !hasHeader("transfer-encoding")) {
            m_headers["transfer-encoding"] = {"chunked"};
            m_use_chunked_encoding = true;
        } else if (getFirstHeaderValue("transfer-encoding") == "chunked") {
            m_use_chunked_encoding = true;
        }
        sendHeaders();
    }

    if (m_use_chunked_encoding) {
        sendChunk(data);
    } else {
        m_conn->send(data);
    }
}

void HTTPResponse::end(const std::string& data) {
    if (m_finished) {
        return;
    }

    if (!m_headers_sent) {
        if (!hasHeader("content-length") && !hasHeader("transfer-encoding")) {
            m_headers["content-length"] = {std::to_string(data.size())};
        } else if (getFirstHeaderValue("transfer-encoding") == "chunked") {
            m_use_chunked_encoding = true;
        }
        sendHeaders();
    }

    if (!data.empty()) {
        if (m_use_chunked_encoding) {
            sendChunk(data);
        } else {
            m_conn->send(data);
        }
    }

    if (m_use_chunked_encoding) {
        m_conn->send("0\r\n\r\n");
    }

    m_finished = true;
    emit("finish");
}

void HTTPResponse::flushHeaders() {
    if (m_finished || m_headers_sent) {
        return;
    }

    if (!hasHeader("content-length") && !hasHeader("transfer-encoding")) {
        m_headers["transfer-encoding"] = {"chunked"};
        m_use_chunked_encoding = true;
    } else if (getFirstHeaderValue("transfer-encoding") == "chunked") {
        m_use_chunked_encoding = true;
    }
    sendHeaders();
}

void HTTPResponse::sendHeaders() {
    if (m_headers_sent) {
        return;
    }

    std::ostringstream ss;
    std::string msg = m_status_message.empty() ? getDefaultStatusMessage(m_status_code) : m_status_message;
    if (m_send_date && !hasHeader("date")) {
        m_headers["date"] = {formatHttpDate()};
    }
    ss << "HTTP/1.1 " << m_status_code << " " << msg << "\r\n";
    for (const auto& pair : m_headers) {
        for (const std::string& value : pair.second) {
            ss << pair.first << ": " << value << "\r\n";
        }
    }
    ss << "\r\n";
    m_conn->send(ss.str());
    m_headers_sent = true;
}

void HTTPResponse::sendChunk(const std::string& data) {
    if (data.empty()) {
        return;
    }

    std::ostringstream chunk;
    chunk << std::hex << data.size() << "\r\n";
    chunk << data << "\r\n";
    m_conn->send(chunk.str());
}

void HTTPResponse::emit(const char* p_event_name) {
    if (m_res_obj.IsEmpty()) {
        return;
    }

    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> res_obj = m_res_obj.Get(p_isolate);
    v8::Local<v8::Value> emit_val;
    if (!res_obj->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) || !emit_val->IsFunction()) {
        return;
    }

    v8::Local<v8::Value> emit_args[1] = {
        v8::String::NewFromUtf8(p_isolate, p_event_name).ToLocalChecked()
    };
    (void)emit_val.As<v8::Function>()->Call(p_context, res_obj, 1, emit_args);
}

void HTTPResponse::scheduleDelete() {
    bool expected = false;
    if (!m_delete_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    enqueueDeferredDelete(this);
}

std::string HTTPResponse::getFirstHeaderValue(const std::string& name) const {
    const std::vector<std::string>* p_values = getHeader(name);
    if (!p_values || p_values->empty()) {
        return "";
    }
    return p_values->front();
}

// HTTP Module Template
v8::Local<v8::ObjectTemplate> HTTP::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->Set(p_isolate, "createServer", v8::FunctionTemplate::New(p_isolate, createServer));
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "METHODS"), getMethods);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "STATUS_CODES"), getStatusCodes);
    tmpl->Set(p_isolate, "request", v8::FunctionTemplate::New(p_isolate, request));
    tmpl->Set(p_isolate, "get", v8::FunctionTemplate::New(p_isolate, get));
    return tmpl;
}

void HTTP::createServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    auto* p_server = new HTTPServer(p_isolate);
    v8::Local<v8::Function> request_handler;
    if (args.Length() > 0 && args[0]->IsFunction()) {
        request_handler = args[0].As<v8::Function>();
    } else if (args.Length() > 1 && args[1]->IsFunction()) {
        request_handler = args[1].As<v8::Function>();
    }
    if (!request_handler.IsEmpty()) {
        p_server->setRequestHandler(request_handler);
    }

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    tmpl->Set(p_isolate, "listen", v8::FunctionTemplate::New(p_isolate, serverListen));
    tmpl->Set(p_isolate, "close", v8::FunctionTemplate::New(p_isolate, serverClose));
    tmpl->Set(p_isolate, "closeAllConnections", v8::FunctionTemplate::New(p_isolate, serverCloseAllConnections));
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "listening"),
                                HTTP::serverGetListening,
                                nullptr);

    v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
    v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Object> ee_obj = ee_ctor->NewInstance(context).ToLocalChecked();

    v8::Local<v8::Object> server_obj = tmpl->NewInstance(context).ToLocalChecked();
    server_obj->SetAlignedPointerInInternalField(0, p_server, v8::kEmbedderDataTypeTagDefault);
    copyObjectProperties(p_isolate, context, ee_obj, server_obj);
    p_server->setJSObject(server_obj);
    args.GetReturnValue().Set(server_obj);
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

void HTTP::serverCloseAllConnections(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_server = unwrapServer(args.This());
    if (p_server) {
        p_server->closeAllConnections();
    }
}

void HTTP::responseWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        int32_t status = 200;
        std::string status_message;
        v8::Local<v8::Object> headers;
        if (args.Length() > 0 && args[0]->IsNumber()) {
            status = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
        }
        if (args.Length() > 1 && args[1]->IsString()) {
            v8::String::Utf8Value status_message_value(args.GetIsolate(), args[1]);
            status_message = std::string(*status_message_value, status_message_value.length());
        } else if (args.Length() > 1 && args[1]->IsObject()) {
            headers = args[1].As<v8::Object>();
        }
        if (args.Length() > 2 && args[2]->IsObject()) {
            headers = args[2].As<v8::Object>();
        }

        if (status_message.empty()) {
            p_response->writeHead(status, headers);
        } else {
            p_response->writeHead(status, status_message, headers);
        }
    }
}

void HTTP::responseSetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 2 && args[0]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        std::vector<std::string> values;
        if (extractHeaderValues(p_isolate, args[1], &values)) {
            p_response->setHeader(std::string(*name, name.length()), values);
        }
    }
}

void HTTP::responseGetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response && args.Length() >= 1 && args[0]->IsString()) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::String::Utf8Value name(p_isolate, args[0]);
        const std::vector<std::string>* p_values = p_response->getHeader(std::string(*name, name.length()));
        if (p_values && !p_values->empty()) {
            if (p_values->size() == 1) {
                args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, p_values->front().c_str()).ToLocalChecked());
            } else {
                v8::Local<v8::Array> values = v8::Array::New(p_isolate, static_cast<int32_t>(p_values->size()));
                v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                for (uint32_t i = 0; i < p_values->size(); i++) {
                    values->Set(context, i, v8::String::NewFromUtf8(p_isolate, (*p_values)[i].c_str()).ToLocalChecked()).Check();
                }
                args.GetReturnValue().Set(values);
            }
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
    if (p_response && args.Length() > 0) {
        std::string data;
        if (readBinaryValue(args.GetIsolate(), args[0], &data)) {
            p_response->write(data);
        }
    }
}

void HTTP::responseEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        std::string data;
        if (args.Length() > 0) {
            (void)readBinaryValue(args.GetIsolate(), args[0], &data);
        }
        p_response->end(data);
    }
}

void HTTP::responseFlushHeaders(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_response = unwrapResponse(args.This());
    if (p_response) {
        p_response->flushHeaders();
    }
}

void HTTP::getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    const char* methods[] = {
        "ACL", "BIND", "CHECKOUT", "CONNECT", "COPY", "DELETE", "GET", "HEAD",
        "LINK", "LOCK", "M-SEARCH", "MERGE", "MKACTIVITY", "MKCALENDAR", "MKCOL",
        "MOVE", "NOTIFY", "OPTIONS", "PATCH", "POST", "PROPFIND", "PROPPATCH",
        "PURGE", "PUT", "QUERY", "REBIND", "REPORT", "SEARCH", "SOURCE",
        "SUBSCRIBE", "TRACE", "UNBIND", "UNLINK", "UNLOCK", "UNSUBSCRIBE"
    };
    constexpr int32_t METHOD_COUNT = sizeof(methods) / sizeof(methods[0]);
    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, METHOD_COUNT);
    for (int32_t i = 0; i < METHOD_COUNT; i++) {
        arr->Set(p_isolate->GetCurrentContext(), i, v8::String::NewFromUtf8(p_isolate, methods[i]).ToLocalChecked()).Check();
    }
    info.GetReturnValue().Set(arr);
}

void HTTP::getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    v8::Local<v8::Object> codes = v8::Object::New(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    const int32_t status_codes[] = {
        100, 101, 102, 103, 200, 201, 202, 203, 204, 205, 206, 207, 208, 226,
        300, 301, 302, 303, 304, 305, 307, 308, 400, 401, 402, 403, 404, 405,
        406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 421,
        422, 423, 424, 425, 426, 429, 431, 451, 500, 501, 502, 503, 504, 505,
        506, 507, 508, 510, 511
    };
    for (int32_t status_code : status_codes) {
        std::string status_key = std::to_string(status_code);
        std::string status_message = getDefaultStatusMessage(status_code);
        codes->Set(
            context,
            v8::String::NewFromUtf8(p_isolate, status_key.c_str()).ToLocalChecked(),
            v8::String::NewFromUtf8(p_isolate, status_message.c_str()).ToLocalChecked()).Check();
    }
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

void HTTP::responseGetHeadersSent(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response) {
        info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), p_response->getHeadersSent()));
    }
}

void HTTP::responseGetFinished(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response) {
        info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), p_response->getFinished()));
    }
}

void HTTP::responseGetSendDate(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response) {
        info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), p_response->getSendDate()));
    }
}

void HTTP::responseSetSendDate(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    HTTPResponse* p_response = unwrapResponse(info.HolderV2());
    if (p_response && value->IsBoolean()) {
        p_response->setSendDate(value.As<v8::Boolean>()->Value());
    }
}

void HTTP::requestDestroy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    HTTPRequest* p_request = unwrapRequest(args.This());
    if (p_request) {
        p_request->destroy();
    }
}

void HTTP::requestGetDestroyed(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPRequest* p_request = unwrapRequest(info.HolderV2());
    if (p_request) {
        info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), p_request->isDestroyed()));
    }
}

void HTTP::serverGetListening(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    HTTPServer* p_server = unwrapServer(info.HolderV2());
    if (p_server) {
        info.GetReturnValue().Set(v8::Boolean::New(info.GetIsolate(), p_server->getListening()));
    }
}

HTTPServer* HTTP::unwrapServer(v8::Local<v8::Object> obj) {
    return unwrapExternalPointer<HTTPServer>(obj);
}

HTTPRequest* HTTP::unwrapRequest(v8::Local<v8::Object> obj) {
    return unwrapExternalPointer<HTTPRequest>(obj);
}

HTTPResponse* HTTP::unwrapResponse(v8::Local<v8::Object> obj) {
    return unwrapExternalPointer<HTTPResponse>(obj);
}

HTTPClientRequest* HTTP::unwrapClientRequest(v8::Local<v8::Object> obj) {
    return unwrapExternalPointer<HTTPClientRequest>(obj);
}

// --- HTTPClientRequest Implementation ---

HTTPClientRequest::HTTPClientRequest(v8::Isolate* p_isolate, const std::string& method, const std::string& host, int32_t port, const std::string& path, v8::Local<v8::Object> headers, v8::Local<v8::Function> callback)
    : p_isolate(p_isolate),
      m_method(method),
      m_host(host),
      m_port(port),
      m_path(path),
      m_status_code(0),
      m_last_header_state(HeaderParseState::NONE),
      m_finished(false),
      m_executed(false),
      m_request_complete(false),
      m_gc_pending(false),
      m_network_ref_active(false),
      m_ref_count(1),
      m_delete_scheduled(false),
      m_error_emitted(false) {
    if (!callback.IsEmpty()) {
        m_response_callback.Reset(p_isolate, callback);
    }

    if (!headers.IsEmpty()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Array> prop_names = headers->GetPropertyNames(context).ToLocalChecked();
        for (uint32_t i = 0; i < prop_names->Length(); ++i) {
            v8::Local<v8::Value> key = prop_names->Get(context, i).ToLocalChecked();
            v8::Local<v8::Value> val = headers->Get(context, key).ToLocalChecked();
            v8::String::Utf8Value key_str(p_isolate, key);
            v8::String::Utf8Value val_str(p_isolate, val);
            setHeader(std::string(*key_str, key_str.length()), std::string(*val_str, val_str.length()));
        }
    }

    if (m_headers.find("host") == m_headers.end()) {
        setHeader("Host", m_host);
    }
    if (m_headers.find("connection") == m_headers.end()) {
        setHeader("Connection", "close");
    }

    llhttp_settings_init(&m_settings);
    m_settings.on_message_begin = onMessageBegin;
    m_settings.on_status = onStatus;
    m_settings.on_header_field = onHeaderField;
    m_settings.on_header_value = onHeaderValue;
    m_settings.on_headers_complete = onHeadersComplete;
    m_settings.on_body = onBody;
    m_settings.on_message_complete = onMessageComplete;

    llhttp_init(&m_parser, HTTP_RESPONSE, &m_settings);
    m_parser.data = this;
}

HTTPClientRequest::~HTTPClientRequest() {
    if (sp_tcp_client) {
        sp_tcp_client->disconnect();
    }
}

void HTTPClientRequest::retain() {
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void HTTPClientRequest::release() {
    int32_t ref_count = m_ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref_count == 0 && m_gc_pending.load(std::memory_order_acquire)) {
        scheduleDelete();
    }
}

void HTTPClientRequest::setHeader(const std::string& name, const std::string& value) {
    std::string normalized_name = normalizeHeaderName(name);
    if (!hasHeader(normalized_name)) {
        m_raw_header_names.push_back(name);
    }
    m_headers[normalized_name] = value;
}

bool HTTPClientRequest::hasHeader(const std::string& name) const {
    return m_headers.find(normalizeHeaderName(name)) != m_headers.end();
}

std::string HTTPClientRequest::getHeader(const std::string& name) const {
    auto it = m_headers.find(normalizeHeaderName(name));
    if (it != m_headers.end()) {
        return it->second;
    }
    return "";
}

void HTTPClientRequest::removeHeader(const std::string& name) {
    std::string normalized_name = normalizeHeaderName(name);
    m_headers.erase(normalized_name);
    for (auto it = m_raw_header_names.begin(); it != m_raw_header_names.end();) {
        if (normalizeHeaderName(*it) == normalized_name) {
            it = m_raw_header_names.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::string> HTTPClientRequest::getHeaderNames() const {
    std::vector<std::string> names;
    names.reserve(m_headers.size());
    for (const auto& pair : m_headers) {
        names.push_back(pair.first);
    }
    return names;
}

std::vector<std::string> HTTPClientRequest::getRawHeaderNames() const {
    return m_raw_header_names;
}

v8::Local<v8::Object> HTTPClientRequest::createObject(v8::Isolate* p_isolate) {
    v8::EscapableHandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    tmpl->Set(p_isolate, "write", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::write));
    tmpl->Set(p_isolate, "end", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::end));
    tmpl->Set(p_isolate, "flushHeaders", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::flushHeaders));
    tmpl->Set(p_isolate, "setHeader", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::setHeader));
    tmpl->Set(p_isolate, "getHeader", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::getHeader));
    tmpl->Set(p_isolate, "hasHeader", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::hasHeader));
    tmpl->Set(p_isolate, "removeHeader", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::removeHeader));
    tmpl->Set(p_isolate, "getHeaders", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::getHeaders));
    tmpl->Set(p_isolate, "getHeaderNames", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::getHeaderNames));
    tmpl->Set(p_isolate, "getRawHeaderNames", v8::FunctionTemplate::New(p_isolate, HTTPClientRequest::getRawHeaderNames));
    
    v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
    v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Object> obj = ee_ctor->NewInstance(context).ToLocalChecked();
    v8::Local<v8::Object> client_obj = tmpl->NewInstance(context).ToLocalChecked();
    client_obj->SetAlignedPointerInInternalField(0, this, v8::kEmbedderDataTypeTagDefault);
    copyObjectProperties(p_isolate, context, obj, client_obj);
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "method"), v8::String::NewFromUtf8(p_isolate, m_method.c_str()).ToLocalChecked()).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "path"), v8::String::NewFromUtf8(p_isolate, m_path.c_str()).ToLocalChecked()).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "host"), v8::String::NewFromUtf8(p_isolate, m_host.c_str()).ToLocalChecked()).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "protocol"), v8::String::NewFromUtf8Literal(p_isolate, "http:")).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "finished"), v8::Boolean::New(p_isolate, false)).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "writableEnded"), v8::Boolean::New(p_isolate, false)).Check();
    client_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "writableFinished"), v8::Boolean::New(p_isolate, false)).Check();

    m_js_object.Reset(p_isolate, client_obj);
    m_js_object.SetWeak(this, weakDeleteClientRequest, v8::WeakCallbackType::kParameter);
    return handle_scope.Escape(client_obj);
}

void HTTPClientRequest::write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self && args.Length() > 0) {
        std::string data;
        if (readBinaryValue(args.GetIsolate(), args[0], &data)) {
            p_self->m_body += data;
        }
    }
}

void HTTPClientRequest::end(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (!p_self) {
        return;
    }
    if (args.Length() > 0) {
        std::string data;
        if (readBinaryValue(args.GetIsolate(), args[0], &data)) {
            p_self->m_body += data;
        }
    }
    p_self->execute();
}

void HTTPClientRequest::flushHeaders(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self) {
        p_self->execute();
    }
}

void HTTPClientRequest::setHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self && args.Length() >= 2 && args[0]->IsString() && args[1]->IsString()) {
        v8::String::Utf8Value name(args.GetIsolate(), args[0]);
        v8::String::Utf8Value value(args.GetIsolate(), args[1]);
        p_self->setHeader(std::string(*name, name.length()), std::string(*value, value.length()));
    }
}

void HTTPClientRequest::getHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self && args.Length() >= 1 && args[0]->IsString()) {
        v8::String::Utf8Value name(args.GetIsolate(), args[0]);
        std::string value = p_self->getHeader(std::string(*name, name.length()));
        if (!value.empty()) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8(args.GetIsolate(), value.c_str()).ToLocalChecked());
        } else {
            args.GetReturnValue().SetNull();
        }
    }
}

void HTTPClientRequest::hasHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self && args.Length() >= 1 && args[0]->IsString()) {
        v8::String::Utf8Value name(args.GetIsolate(), args[0]);
        args.GetReturnValue().Set(v8::Boolean::New(args.GetIsolate(), p_self->hasHeader(std::string(*name, name.length()))));
    }
}

void HTTPClientRequest::removeHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (p_self && args.Length() >= 1 && args[0]->IsString()) {
        v8::String::Utf8Value name(args.GetIsolate(), args[0]);
        p_self->removeHeader(std::string(*name, name.length()));
    }
}

void HTTPClientRequest::getHeaders(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (!p_self) {
        return;
    }

    v8::Local<v8::Object> headers = v8::Object::New(args.GetIsolate());
    v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
    for (const auto& pair : p_self->getHeaders()) {
        headers->Set(context,
                     v8::String::NewFromUtf8(args.GetIsolate(), pair.first.c_str()).ToLocalChecked(),
                     v8::String::NewFromUtf8(args.GetIsolate(), pair.second.c_str()).ToLocalChecked()).Check();
    }
    args.GetReturnValue().Set(headers);
}

void HTTPClientRequest::getHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (!p_self) {
        return;
    }

    std::vector<std::string> names = p_self->getHeaderNames();
    v8::Local<v8::Array> result = v8::Array::New(args.GetIsolate(), names.size());
    v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
    for (uint32_t i = 0; i < names.size(); i++) {
        result->Set(context, i, v8::String::NewFromUtf8(args.GetIsolate(), names[i].c_str()).ToLocalChecked()).Check();
    }
    args.GetReturnValue().Set(result);
}

void HTTPClientRequest::getRawHeaderNames(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* p_self = unwrapExternalPointer<HTTPClientRequest>(args.This());
    if (!p_self) {
        return;
    }

    std::vector<std::string> names = p_self->getRawHeaderNames();
    v8::Local<v8::Array> result = v8::Array::New(args.GetIsolate(), names.size());
    v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
    for (uint32_t i = 0; i < names.size(); i++) {
        result->Set(context, i, v8::String::NewFromUtf8(args.GetIsolate(), names[i].c_str()).ToLocalChecked()).Check();
    }
    args.GetReturnValue().Set(result);
}

void HTTPClientRequest::releaseNetworkReference() {
    bool expected = true;
    if (!m_network_ref_active.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    release();
}

void HTTPClientRequest::scheduleDelete() {
    bool expected = false;
    if (!m_delete_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    enqueueDeferredDelete(this);
}

void HTTPClientRequest::execute() {
    if (m_executed) {
        return;
    }
    m_executed = true;
    retain();
    m_network_ref_active.store(true, std::memory_order_release);

    up_loop_thread = std::make_unique<trantor::EventLoopThread>();
    up_loop_thread->run();
    trantor::EventLoop* p_loop = up_loop_thread->getLoop();

    trantor::InetAddress serverAddr(m_host, m_port);
    sp_tcp_client = std::make_shared<trantor::TcpClient>(p_loop, serverAddr, "HTTPClient_" + m_host);

    std::ostringstream req;
    req << m_method << " " << m_path << " HTTP/1.1\r\n";
    if (!m_body.empty() && m_headers.find("content-length") == m_headers.end()) {
        m_headers["content-length"] = std::to_string(m_body.length());
    }
    for (const auto& pair : m_headers) {
        req << pair.first << ": " << pair.second << "\r\n";
    }
    req << "\r\n";
    req << m_body;

    std::string request_str = req.str();

    sp_tcp_client->setConnectionCallback([this, request_str](const trantor::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send(request_str);
        } else {
            if (!m_request_complete && !m_error_emitted) {
                m_error_emitted = true;
                retain();
                Task* p_task = new Task();
                p_task->m_runner = [this](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
                    if (!m_js_object.IsEmpty()) {
                        v8::HandleScope handle_scope(p_isolate);
                        v8::Local<v8::Object> js_obj = m_js_object.Get(p_isolate);
                        v8::Local<v8::Value> emit_val;
                        if (js_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                            v8::Local<v8::Value> emit_args[2] = {
                                v8::String::NewFromUtf8Literal(p_isolate, "error"),
                                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "socket hang up"))
                            };
                            (void)emit_val.As<v8::Function>()->Call(context, js_obj, 2, emit_args);
                        }
                    }
                    release();
                };
                TaskQueue::getInstance().enqueue(p_task);
            }
            releaseNetworkReference();
        }
    });

    sp_tcp_client->setMessageCallback([this](const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
        std::string data(buf->peek(), buf->readableBytes());
        buf->retrieveAll();
        
        llhttp_errno_t err = llhttp_execute(&m_parser, data.data(), data.length());
        if (err != HPE_OK) {
            std::cerr << "llhttp parse error in client: " << llhttp_errno_name(err) << std::endl;
            conn->forceClose();
        }
    });

    sp_tcp_client->connect();
}

int32_t HTTPClientRequest::onMessageBegin(llhttp_t* p_parser) {
    return 0;
}

int32_t HTTPClientRequest::onStatus(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    p_self->m_status_code = p_parser->status_code;
    p_self->m_status_message += std::string(p_at, length);
    return 0;
}

int32_t HTTPClientRequest::onHeaderField(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    p_self->appendResponseHeaderField(p_at, length);
    return 0;
}

int32_t HTTPClientRequest::onHeaderValue(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    p_self->appendResponseHeaderValue(p_at, length);
    return 0;
}

int32_t HTTPClientRequest::onHeadersComplete(llhttp_t* p_parser) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    p_self->finishPendingResponseHeader();
    p_self->retain();
    
    Task* p_task = new Task();
    p_task->m_runner = [p_self](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
        p_self->emitResponse();
        p_self->release();
    };
    TaskQueue::getInstance().enqueue(p_task);
    
    return 0;
}

int32_t HTTPClientRequest::onBody(llhttp_t* p_parser, const char* p_at, size_t length) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    std::string data(p_at, length);
    p_self->retain();
    
    Task* p_task = new Task();
    p_task->m_runner = [p_self, data](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
        p_self->emitData(data);
        p_self->release();
    };
    TaskQueue::getInstance().enqueue(p_task);
    
    return 0;
}

int32_t HTTPClientRequest::onMessageComplete(llhttp_t* p_parser) {
    auto* p_self = static_cast<HTTPClientRequest*>(p_parser->data);
    p_self->retain();
    
    Task* p_task = new Task();
    p_task->m_runner = [p_self](v8::Isolate* p_isolate, v8::Local<v8::Context> context, Task* p_task) {
        p_self->emitEnd();
        p_self->release();
    };
    TaskQueue::getInstance().enqueue(p_task);
    
    return 0;
}

void HTTPClientRequest::appendResponseHeaderField(const char* p_data, size_t length) {
    if (m_last_header_state == HeaderParseState::VALUE) {
        finishPendingResponseHeader();
    }
    m_current_header_field.append(p_data, length);
    m_last_header_state = HeaderParseState::FIELD;
}

void HTTPClientRequest::appendResponseHeaderValue(const char* p_data, size_t length) {
    m_current_header_value.append(p_data, length);
    m_last_header_state = HeaderParseState::VALUE;
}

void HTTPClientRequest::finishPendingResponseHeader() {
    if (!m_current_header_field.empty()) {
        m_raw_response_headers.push_back(m_current_header_field);
        m_raw_response_headers.push_back(m_current_header_value);
    }
    m_current_header_field.clear();
    m_current_header_value.clear();
    m_last_header_state = HeaderParseState::NONE;
}

void HTTPClientRequest::emitResponse() {
    if (m_js_object.IsEmpty()) {
        return;
    }

    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
    v8::Local<v8::Function> ee_ctor = ee_tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Object> res_obj = ee_ctor->NewInstance(context).ToLocalChecked();

    res_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "statusCode"), v8::Integer::New(p_isolate, m_status_code)).Check();
    res_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "statusMessage"), v8::String::NewFromUtf8(p_isolate, m_status_message.c_str()).ToLocalChecked()).Check();

    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    for (size_t i = 0; i < m_raw_response_headers.size(); i += 2) {
        std::string lower_name = normalizeHeaderName(m_raw_response_headers[i]);
        headers->Set(context,
                    v8::String::NewFromUtf8(p_isolate, lower_name.c_str()).ToLocalChecked(),
                    v8::String::NewFromUtf8(p_isolate, m_raw_response_headers[i+1].c_str()).ToLocalChecked()).Check();
    }
    res_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "headers"), headers).Check();
    m_response_obj.Reset(p_isolate, res_obj);

    v8::Local<v8::Object> js_obj = m_js_object.Get(p_isolate);
    v8::Local<v8::Value> emit_val;
    if (!js_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) || !emit_val->IsFunction()) {
        return;
    }

    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
    v8::Local<v8::Function> response_callback;
    bool has_response_callback = false;
    if (!m_response_callback.IsEmpty()) {
        response_callback = m_response_callback.Get(p_isolate);
        has_response_callback = true;
    }

    v8::Local<v8::Value> emit_args[2] = {
        v8::String::NewFromUtf8Literal(p_isolate, "response"),
        res_obj
    };
    (void)emit_fn->Call(context, js_obj, 2, emit_args);

    if (has_response_callback) {
        v8::Local<v8::Value> cb_args[1] = { res_obj };
        (void)response_callback->Call(context, context->Global(), 1, cb_args);
    }
}

void HTTPClientRequest::emitData(const std::string& data) {
    if (m_response_obj.IsEmpty()) {
        return;
    }

    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::Object> res_obj = m_response_obj.Get(p_isolate);
    v8::Local<v8::Value> emit_val;
    if (!res_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) || !emit_val->IsFunction()) {
        return;
    }

    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();

    v8::Local<v8::Value> emit_args[2] = {
        v8::String::NewFromUtf8Literal(p_isolate, "data"),
        createBufferValue(p_isolate, data)
    };
    (void)emit_fn->Call(context, res_obj, 2, emit_args);
}

void HTTPClientRequest::emitEnd() {
    if (m_response_obj.IsEmpty()) {
        return;
    }

    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::Object> res_obj = m_response_obj.Get(p_isolate);
    v8::Local<v8::Value> emit_val;
    if (!res_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) || !emit_val->IsFunction()) {
        return;
    }

    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();

    v8::Local<v8::Value> emit_args[1] = {
        v8::String::NewFromUtf8Literal(p_isolate, "end")
    };
    (void)emit_fn->Call(context, res_obj, 1, emit_args);
    m_finished = true;
    m_request_complete = true;

    if (!m_js_object.IsEmpty()) {
        v8::Local<v8::Object> js_obj = m_js_object.Get(p_isolate);
        js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "finished"), v8::Boolean::New(p_isolate, true)).Check();
        js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "writableEnded"), v8::Boolean::New(p_isolate, true)).Check();
        js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "writableFinished"), v8::Boolean::New(p_isolate, true)).Check();
    }
    
    m_js_object.Reset();
    m_response_obj.Reset();
    m_response_callback.Reset();
    if (sp_tcp_client) {
        sp_tcp_client->disconnect();
    }
}

void HTTP::request(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "url or options required")));
        return;
    }

    std::string method = "GET";
    std::string host = "localhost";
    int32_t port = 80;
    std::string path = "/";
    v8::Local<v8::Object> headers = v8::Object::New(p_isolate);
    v8::Local<v8::Function> callback;

    if (args[0]->IsString()) {
        v8::String::Utf8Value url_v(p_isolate, args[0]);
        std::string url_str = *url_v;

        if (url_str.find("http://") == 0) {
            url_str = url_str.substr(7);
        }
        size_t slash_pos = url_str.find_first_of('/');
        if (slash_pos != std::string::npos) {
            path = url_str.substr(slash_pos);
            host = url_str.substr(0, slash_pos);
        } else {
            host = url_str;
            path = "/";
        }
        
        size_t colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            if (!tryParsePort(host.substr(colon_pos + 1), &port)) {
                p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid port in URL")));
                return;
            }
            host = host.substr(0, colon_pos);
        }

        if (args.Length() > 1 && args[1]->IsObject() && !args[1]->IsFunction()) {
            v8::Local<v8::Object> opts = args[1].As<v8::Object>();
            v8::Local<v8::Value> m;
            if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "method")).ToLocal(&m) && m->IsString()) {
                v8::String::Utf8Value mv(p_isolate, m);
                method = *mv;
            }
            v8::Local<v8::Value> hdrs;
            if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "headers")).ToLocal(&hdrs) && hdrs->IsObject()) {
                headers = hdrs.As<v8::Object>();
            }
            if (args.Length() > 2 && args[2]->IsFunction()) {
                callback = args[2].As<v8::Function>();
            }
        } else if (args.Length() > 1 && args[1]->IsFunction()) {
            callback = args[1].As<v8::Function>();
        }
    } else if (args[0]->IsObject()) {
        v8::Local<v8::Object> opts = args[0].As<v8::Object>();
        v8::Local<v8::Value> h;
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "host")).ToLocal(&h) && h->IsString()) {
            v8::String::Utf8Value hv(p_isolate, h);
            host = *hv;
        } else if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "hostname")).ToLocal(&h) && h->IsString()) {
            v8::String::Utf8Value hv(p_isolate, h);
            host = *hv;
        }
        v8::Local<v8::Value> po;
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "port")).ToLocal(&po) && po->IsNumber()) {
            port = po->Int32Value(context).ToChecked();
        }
        v8::Local<v8::Value> pa;
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "path")).ToLocal(&pa) && pa->IsString()) {
            v8::String::Utf8Value pav(p_isolate, pa);
            path = *pav;
        }
        v8::Local<v8::Value> m;
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "method")).ToLocal(&m) && m->IsString()) {
            v8::String::Utf8Value mv(p_isolate, m);
            method = *mv;
        }
        v8::Local<v8::Value> hdrs;
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "headers")).ToLocal(&hdrs) && hdrs->IsObject()) {
            headers = hdrs.As<v8::Object>();
        }

        if (args.Length() > 1 && args[1]->IsFunction()) {
            callback = args[1].As<v8::Function>();
        }
    }

    HTTPClientRequest* p_client = new HTTPClientRequest(p_isolate, method, host, port, path, headers, callback);
    v8::Local<v8::Object> req_obj = p_client->createObject(p_isolate);
    args.GetReturnValue().Set(req_obj);
}

void HTTP::get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    request(args);
    if (!args.GetReturnValue().Get()->IsNullOrUndefined()) {
        v8::Local<v8::Object> req_obj = args.GetReturnValue().Get().As<v8::Object>();
        v8::Local<v8::Function> end_fn = req_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "end")).ToLocalChecked().As<v8::Function>();
        (void)end_fn->Call(context, req_obj, 0, nullptr);
    }
}

} // namespace module
} // namespace z8
