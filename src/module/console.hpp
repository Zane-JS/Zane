#ifndef Z8_CONSOLE_H
#define Z8_CONSOLE_H

#include "v8.h"
#include <cstdint>

namespace z8 {
namespace module {

class Console {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

  private:
    static void log(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void error(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void warn(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void info(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void assert_(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void count(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void countReset(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void dir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void group(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void groupCollapsed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void groupEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void time(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void timeLog(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void timeEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void trace(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void clear(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void write(const v8::FunctionCallbackInfo<v8::Value>& args, const char* p_prefix, bool is_error = false);
    static void adaptiveFlush(FILE* p_out);

  private:
    static int32_t m_indentation_level;
};

} // namespace module
} // namespace z8

#endif // Z8_CONSOLE_H
