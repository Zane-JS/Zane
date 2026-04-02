#ifndef Z8_MODULE_UTIL_H
#define Z8_MODULE_UTIL_H

#include "v8.h"
#include <cstdio>
#include <string>

namespace z8 {
namespace module {

class Util {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    static void format(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void promisify(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void callbackify(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inherits(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inspect(const v8::FunctionCallbackInfo<v8::Value>& args);
    static std::string inspectInternal(v8::Isolate* p_isolate,
                                       v8::Local<v8::Value> value,
                                       int32_t depth = 2,
                                       int32_t current_depth = 0,
                                       bool colors = false);
    static bool shouldLogWithColors(FILE* p_stream);

    // util.types
    static v8::Local<v8::ObjectTemplate> createTypesTemplate(v8::Isolate* p_isolate);
    static void isAnyArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isArgumentsObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isAsyncFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isBigInt64Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isBigUint64Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isBooleanObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isBoxedPrimitive(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isDataView(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isDate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isExternal(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isFloat32Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isFloat64Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isGeneratorFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isGeneratorObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isInt8Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isInt16Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isInt32Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isMap(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isMapIterator(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isModuleNamespaceObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isNativeError(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isNumberObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isProxy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isRegExp(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isSet(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isSetIterator(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isSharedArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isStringObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isSymbolObject(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isTypedArray(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isUint8Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isUint8ClampedArray(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isUint16Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isUint32Array(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isWeakMap(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isWeakSet(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_UTIL_H
