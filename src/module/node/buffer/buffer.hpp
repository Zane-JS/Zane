#ifndef Z8_MODULE_BUFFER_H
#define Z8_MODULE_BUFFER_H

#include "v8.h"

namespace z8 {
namespace module {

class Buffer {
  public:
    static v8::Local<v8::FunctionTemplate> createTemplate(v8::Isolate* p_isolate);
    static void initialize(v8::Isolate* p_isolate, v8::Local<v8::Context> context);

    // Static methods
    static void alloc(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void allocUnsafe(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void allocUnsafeSlow(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void from(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void concat(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isEncoding(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void byteLength(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void compare(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void atob(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void btoa(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isAscii(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isUtf8(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Instance methods
    static void toString(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void write(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fill(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void copy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void slice(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void subarray(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void equals(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void compare_instance(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void indexOf(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lastIndexOf(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void includes(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void toJSON(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void swap16(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void swap32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void swap64(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Read integer methods
    static void readUInt8(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readInt8(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readFloatBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readFloatLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readDoubleBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readDoubleLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readBigInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readBigInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readBigUInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readBigUInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readIntBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readIntLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUIntBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readUIntLE(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Write integer methods
    static void writeUInt8(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeInt8(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeFloatBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeFloatLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeDoubleBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeDoubleLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeBigInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeBigInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeBigUInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeBigUInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeIntBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeIntLE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUIntBE(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeUIntLE(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Internal helpers
    static v8::Local<v8::Uint8Array> createBuffer(v8::Isolate* p_isolate, size_t length);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_BUFFER_H
