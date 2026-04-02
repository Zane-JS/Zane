#ifndef Z8_MODULE_PROCESS_H
#define Z8_MODULE_PROCESS_H

#include "v8.h"
#include <map>
#include <string>

namespace z8 {
namespace module {

class Process {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    // Initializer to be called when creating the global 'process' object
    static v8::Local<v8::Object> createObject(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    // Setup global state (call once)
    static void setArgv(int32_t argc, char* argv[]);

    private:
    static void cwd(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chdir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void exit(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void uptime(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void nextTick(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void memoryUsage(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void hrtime(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void hrtimeBigInt(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void kill(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getTitle(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void setTitle(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    static void umask(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void cpuUsage(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void resourceUsage(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Event Emitter (Stubs for now)
    static void on(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void once(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void off(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void emit(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Stream helpers
    static void stdoutWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void stderrWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void stdinRead(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Internal helpers
    static v8::Local<v8::Object> createEnvObject(v8::Isolate* p_isolate, v8::Local<v8::Context> context);
    static std::map<std::string, std::string> loadDotEnv();
    static std::string getExecPath();

    static std::vector<std::string> m_argv;
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_PROCESS_H
