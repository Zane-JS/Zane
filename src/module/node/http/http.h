#ifndef Z8_MODULE_HTTP_H
#define Z8_MODULE_HTTP_H

#include "v8.h"

namespace z8 {
namespace module {

class HTTP {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
    
    // Server methods
    static void createServer(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Client methods
    static void request(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void get(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Constants
    static void getMethods(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void getStatusCodes(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_HTTP_H
