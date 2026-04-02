#ifndef Z8_MODULE_OS_H
#define Z8_MODULE_OS_H

#include "v8.h"

namespace z8 {
namespace module {

class OS {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    static void arch(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void cpus(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void freemem(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void homedir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void hostname(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void loadavg(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void networkInterfaces(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void platform(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void release(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void tmpdir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void totalmem(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void type(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void uptime(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void userInfo(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void version(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getPriority(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setPriority(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_OS_H
