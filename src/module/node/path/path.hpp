#ifndef Z8_MODULE_PATH_H
#define Z8_MODULE_PATH_H

#include "v8.h"

namespace z8 {
namespace module {

class Path {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    static void resolve(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void join(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void normalize(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isAbsolute(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void relative(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void dirname(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void basename(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void extname(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void parse(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void format(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void toNamespacedPath(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Posix specific
    static void resolvePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void joinPosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void normalizePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isAbsolutePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void relativePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void dirnamePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void basenamePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void extnamePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void parsePosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void formatPosix(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void toNamespacedPathPosix(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Win32 specific
    static void resolveWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void joinWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void normalizeWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isAbsoluteWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void relativeWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void dirnameWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void basenameWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void extnameWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void parseWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void formatWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void toNamespacedPathWin32(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_PATH_H
