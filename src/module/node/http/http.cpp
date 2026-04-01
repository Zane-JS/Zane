#include "http.h"
#include <string>

namespace z8 {
namespace module {

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
    
    // TODO: Implement with Drogon
    p_isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(p_isolate, "http.createServer() not yet implemented")));
}

void HTTP::request(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    // TODO: Implement with Drogon
    p_isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(p_isolate, "http.request() not yet implemented")));
}

void HTTP::get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    
    // TODO: Implement with Drogon
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

} // namespace module
} // namespace z8
