#include "buffer.hpp"
#include <cstring>
#include <vector>

namespace zane {
namespace module {

// Helper for Hex
static int32_t hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string bytesToHex(const uint8_t* p_data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string res;
    res.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        res += hex_chars[p_data[i] >> 4];
        res += hex_chars[p_data[i] & 0x0F];
    }
    return res;
}

// Helper for Base64 (Basic)
static const std::string base64_chars = 
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

static std::string bytesToBase64(const uint8_t* p_data, size_t len) {
    std::string res;
    int32_t i = 0;
    int32_t j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];

    while (len--) {
        char_array_3[i++] = *(p_data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for(i = 0; (i <4) ; i++)
                res += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) res += base64_chars[char_array_4[j]];
        while((i++ < 3)) res += '=';
    }
    return res;
}

static std::vector<uint8_t> base64ToBytes(std::string str) {
    // Handle base64url
    for (char& c : str) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Remove padding if needed (basic version)
    // Note: The original code does not explicitly remove padding,
    // but the loop condition `(str[in_] != '=')` handles it implicitly.

    int32_t in_len = static_cast<int32_t>(str.size());
    int32_t i = 0;
    int32_t j = 0;
    int32_t in_ = 0;
    uint8_t char_array_4[4], char_array_3[3];
    std::vector<uint8_t> ret;

    while (in_len-- && ( str[in_] != '=') && (isalnum(str[in_]) || (str[in_] == '+') || (str[in_] == '/'))) {
        char_array_4[i++] = str[in_]; in_++;
        if (i == 4) {
            for (i = 0; i <4; i++)
                char_array_4[i] = static_cast<uint8_t>(base64_chars.find(char_array_4[i]));

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; (i < 3); i++) ret.push_back(char_array_3[i]);
            i = 0;
        }
    }

    if (i) {
        for (j = i; j <4; j++) char_array_4[j] = 0;
        for (j = 0; j <4; j++) char_array_4[j] = (uint8_t)base64_chars.find(char_array_4[j]);
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

v8::Local<v8::FunctionTemplate> Buffer::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, from);

    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Buffer"));

    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "alloc"), v8::FunctionTemplate::New(p_isolate, alloc));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "allocUnsafe"), v8::FunctionTemplate::New(p_isolate, allocUnsafe));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "allocUnsafeSlow"), v8::FunctionTemplate::New(p_isolate, allocUnsafeSlow));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "from"), v8::FunctionTemplate::New(p_isolate, from));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "concat"), v8::FunctionTemplate::New(p_isolate, concat));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isBuffer"), v8::FunctionTemplate::New(p_isolate, isBuffer));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isEncoding"), v8::FunctionTemplate::New(p_isolate, isEncoding));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "byteLength"), v8::FunctionTemplate::New(p_isolate, byteLength));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "compare"), v8::FunctionTemplate::New(p_isolate, compare));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "atob"), v8::FunctionTemplate::New(p_isolate, atob));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "btoa"), v8::FunctionTemplate::New(p_isolate, btoa));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isAscii"), v8::FunctionTemplate::New(p_isolate, isAscii));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isUtf8"), v8::FunctionTemplate::New(p_isolate, isUtf8));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "poolSize"), v8::Integer::New(p_isolate, 8192));

    // Prototype methods
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toString"), v8::FunctionTemplate::New(p_isolate, toString));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, write));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "fill"), v8::FunctionTemplate::New(p_isolate, fill));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "copy"), v8::FunctionTemplate::New(p_isolate, copy));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "slice"), v8::FunctionTemplate::New(p_isolate, slice));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "subarray"), v8::FunctionTemplate::New(p_isolate, subarray));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "equals"), v8::FunctionTemplate::New(p_isolate, equals));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "compare"), v8::FunctionTemplate::New(p_isolate, compare_instance));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "indexOf"), v8::FunctionTemplate::New(p_isolate, indexOf));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "lastIndexOf"), v8::FunctionTemplate::New(p_isolate, lastIndexOf));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "includes"), v8::FunctionTemplate::New(p_isolate, includes));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toJSON"), v8::FunctionTemplate::New(p_isolate, toJSON));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "swap16"), v8::FunctionTemplate::New(p_isolate, swap16));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "swap32"), v8::FunctionTemplate::New(p_isolate, swap32));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "swap64"), v8::FunctionTemplate::New(p_isolate, swap64));
    // Read numeric
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUInt8"), v8::FunctionTemplate::New(p_isolate, readUInt8));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readInt8"), v8::FunctionTemplate::New(p_isolate, readInt8));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUInt16BE"), v8::FunctionTemplate::New(p_isolate, readUInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUInt16LE"), v8::FunctionTemplate::New(p_isolate, readUInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readInt16BE"), v8::FunctionTemplate::New(p_isolate, readInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readInt16LE"), v8::FunctionTemplate::New(p_isolate, readInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUInt32BE"), v8::FunctionTemplate::New(p_isolate, readUInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUInt32LE"), v8::FunctionTemplate::New(p_isolate, readUInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readInt32BE"), v8::FunctionTemplate::New(p_isolate, readInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readInt32LE"), v8::FunctionTemplate::New(p_isolate, readInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readFloatBE"), v8::FunctionTemplate::New(p_isolate, readFloatBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readFloatLE"), v8::FunctionTemplate::New(p_isolate, readFloatLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readDoubleBE"), v8::FunctionTemplate::New(p_isolate, readDoubleBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readDoubleLE"), v8::FunctionTemplate::New(p_isolate, readDoubleLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigInt64BE"), v8::FunctionTemplate::New(p_isolate, readBigInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigInt64LE"), v8::FunctionTemplate::New(p_isolate, readBigInt64LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigUInt64BE"), v8::FunctionTemplate::New(p_isolate, readBigUInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigUInt64LE"), v8::FunctionTemplate::New(p_isolate, readBigUInt64LE));
    // Variable-length read/write
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readIntBE"), v8::FunctionTemplate::New(p_isolate, readIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readIntLE"), v8::FunctionTemplate::New(p_isolate, readIntLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUIntBE"), v8::FunctionTemplate::New(p_isolate, readUIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUIntLE"), v8::FunctionTemplate::New(p_isolate, readUIntLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeIntBE"), v8::FunctionTemplate::New(p_isolate, writeIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeIntLE"), v8::FunctionTemplate::New(p_isolate, writeIntLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUIntBE"), v8::FunctionTemplate::New(p_isolate, writeUIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUIntLE"), v8::FunctionTemplate::New(p_isolate, writeUIntLE));
    // Lowercase aliases (readUint* = readUInt*, writeUint* = writeUInt*)
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUint8"),    v8::FunctionTemplate::New(p_isolate, readUInt8));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUint16BE"), v8::FunctionTemplate::New(p_isolate, readUInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUint16LE"), v8::FunctionTemplate::New(p_isolate, readUInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUint32BE"), v8::FunctionTemplate::New(p_isolate, readUInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUint32LE"), v8::FunctionTemplate::New(p_isolate, readUInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigUint64BE"), v8::FunctionTemplate::New(p_isolate, readBigUInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readBigUint64LE"), v8::FunctionTemplate::New(p_isolate, readBigUInt64LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUintBE"),   v8::FunctionTemplate::New(p_isolate, readUIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "readUintLE"),   v8::FunctionTemplate::New(p_isolate, readUIntLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUint8"),    v8::FunctionTemplate::New(p_isolate, writeUInt8));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUint16BE"), v8::FunctionTemplate::New(p_isolate, writeUInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUint16LE"), v8::FunctionTemplate::New(p_isolate, writeUInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUint32BE"), v8::FunctionTemplate::New(p_isolate, writeUInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUint32LE"), v8::FunctionTemplate::New(p_isolate, writeUInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigUint64BE"), v8::FunctionTemplate::New(p_isolate, writeBigUInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigUint64LE"), v8::FunctionTemplate::New(p_isolate, writeBigUInt64LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUintBE"),   v8::FunctionTemplate::New(p_isolate, writeUIntBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUintLE"),   v8::FunctionTemplate::New(p_isolate, writeUIntLE));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeInt8"), v8::FunctionTemplate::New(p_isolate, writeInt8));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUInt16BE"), v8::FunctionTemplate::New(p_isolate, writeUInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUInt16LE"), v8::FunctionTemplate::New(p_isolate, writeUInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeInt16BE"), v8::FunctionTemplate::New(p_isolate, writeInt16BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeInt16LE"), v8::FunctionTemplate::New(p_isolate, writeInt16LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUInt32BE"), v8::FunctionTemplate::New(p_isolate, writeUInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeUInt32LE"), v8::FunctionTemplate::New(p_isolate, writeUInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeInt32BE"), v8::FunctionTemplate::New(p_isolate, writeInt32BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeInt32LE"), v8::FunctionTemplate::New(p_isolate, writeInt32LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeFloatBE"), v8::FunctionTemplate::New(p_isolate, writeFloatBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeFloatLE"), v8::FunctionTemplate::New(p_isolate, writeFloatLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeDoubleBE"), v8::FunctionTemplate::New(p_isolate, writeDoubleBE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeDoubleLE"), v8::FunctionTemplate::New(p_isolate, writeDoubleLE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigInt64BE"), v8::FunctionTemplate::New(p_isolate, writeBigInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigInt64LE"), v8::FunctionTemplate::New(p_isolate, writeBigInt64LE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigUInt64BE"), v8::FunctionTemplate::New(p_isolate, writeBigUInt64BE));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeBigUInt64LE"), v8::FunctionTemplate::New(p_isolate, writeBigUInt64LE));

    return tmpl;
}

void Buffer::initialize(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::Local<v8::FunctionTemplate> tmpl = createTemplate(p_isolate);
    v8::Local<v8::Function> buffer_fn = tmpl->GetFunction(context).ToLocalChecked();
    
    // Set Buffer as a global
    context->Global()->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer"), buffer_fn).Check();

    // Set constants
    v8::Local<v8::Object> constants = v8::Object::New(p_isolate);
    constants->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "MAX_LENGTH"), v8::Number::New(p_isolate, 4294967296.0)).Check(); // 4GB
    constants->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "MAX_STRING_LENGTH"), v8::Number::New(p_isolate, 1073741823.0)).Check(); // ~1GB
    buffer_fn->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "constants"), constants).Check();

    // In Node.js, Buffer.prototype inherits from Uint8Array.prototype
    // We can try to set the prototype of Buffer.prototype to Uint8Array.prototype
    v8::Local<v8::Value> u8_proto = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Uint8Array"))
        .ToLocalChecked().As<v8::Function>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
    
    v8::Local<v8::Value> buffer_proto = buffer_fn->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
    buffer_proto.As<v8::Object>()->SetPrototypeV2(context, u8_proto).Check();
}

v8::Local<v8::Uint8Array> Buffer::createBuffer(v8::Isolate* p_isolate, size_t length) {
    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, length);
    v8::Local<v8::Uint8Array> ui = v8::Uint8Array::New(ab, 0, length);
    
    // Set the prototype to Buffer.prototype so it gets the Buffer methods
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
    if (buffer_val->IsFunction()) {
        v8::Local<v8::Value> proto = buffer_val.As<v8::Function>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
        ui->SetPrototypeV2(context, proto).Check();
    }
    
    return ui;
}

void Buffer::alloc(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsNumber()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"size\" argument must be of type number")));
        return;
    }

    size_t length = static_cast<size_t>(args[0]->IntegerValue(p_isolate->GetCurrentContext()).FromMaybe(0));
    v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, length);

    // Zero-fill first
    void* p_raw = ui->Buffer()->GetBackingStore()->Data();
    memset(p_raw, 0, length);

    if (args.Length() > 1 && length > 0) {
        // Simulate fill by delegating through the fill method
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::HandleScope handle_scope(p_isolate);
        uint8_t* p_data = static_cast<uint8_t*>(p_raw);
        if (args[1]->IsNumber()) {
            uint8_t fill_val = static_cast<uint8_t>(args[1]->Uint32Value(context).FromMaybe(0));
            memset(p_data, fill_val, length);
        } else if (args[1]->IsString()) {
            std::string enc = "utf8";
            if (args.Length() > 2 && args[2]->IsString()) {
                v8::String::Utf8Value e(p_isolate, args[2]);
                enc = *e;
            }
            v8::String::Utf8Value fill_str(p_isolate, args[1]);
            if (fill_str.length() > 0) {
                for (size_t i = 0; i < length; ) {
                    size_t to_copy = std::min(static_cast<size_t>(fill_str.length()), length - i);
                    memcpy(p_data + i, *fill_str, to_copy);
                    i += to_copy;
                }
            }
        } else if (args[1]->IsUint8Array()) {
            v8::Local<v8::Uint8Array> fill_buf = args[1].As<v8::Uint8Array>();
            size_t fill_len = fill_buf->ByteLength();
            if (fill_len > 0) {
                uint8_t* p_fill = static_cast<uint8_t*>(fill_buf->Buffer()->GetBackingStore()->Data()) + fill_buf->ByteOffset();
                for (size_t i = 0; i < length; ) {
                    size_t to_copy = std::min(fill_len, length - i);
                    memcpy(p_data + i, p_fill, to_copy);
                    i += to_copy;
                }
            }
        }
    }

    args.GetReturnValue().Set(ui);
}

void Buffer::allocUnsafe(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // For now, same as alloc in Zane for simplicity
    alloc(args);
}

void Buffer::from(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The first argument must be one of type string, Buffer, ArrayBuffer, Array, or Array-like object.")));
        return;
    }

    v8::Local<v8::Value> input = args[0];

    // Case 1: String
    if (input->IsString()) {
        v8::String::Utf8Value str(p_isolate, input);
        std::string encoding = "utf8";
        if (args.Length() > 1 && args[1]->IsString()) {
            v8::String::Utf8Value enc_str(p_isolate, args[1]);
            encoding = *enc_str;
        }

        if (encoding == "hex") {
            std::string hex_str(*str);
            size_t len = hex_str.length() / 2;
            v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, len);
            uint8_t* p_data = static_cast<uint8_t*>(ui->Buffer()->GetBackingStore()->Data());
            for (size_t i = 0; i < len; i++) {
                int32_t high = hexValue(hex_str[i * 2]);
                int32_t low  = hexValue(hex_str[i * 2 + 1]);
                if (high == -1 || low == -1) break;
                p_data[i] = static_cast<uint8_t>((high << 4) | low);
            }
            args.GetReturnValue().Set(ui);
            return;
        } else if (encoding == "base64" || encoding == "base64url") {
            std::vector<uint8_t> bytes = base64ToBytes(*str);
            v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, bytes.size());
            memcpy(ui->Buffer()->GetBackingStore()->Data(), bytes.data(), bytes.size());
            args.GetReturnValue().Set(ui);
            return;
        } else if (encoding == "latin1" || encoding == "binary" || encoding == "ascii") {
            size_t len = str.length();
            v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, len);
            memcpy(ui->Buffer()->GetBackingStore()->Data(), *str, len);
            args.GetReturnValue().Set(ui);
            return;
        }

        size_t len = str.length();
        v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, len);
        memcpy(ui->Buffer()->GetBackingStore()->Data(), *str, len);
        args.GetReturnValue().Set(ui);
        return;
    }

    // Case 2: ArrayBuffer or SharedArrayBuffer
    if (input->IsArrayBuffer() || input->IsSharedArrayBuffer()) {
        v8::Local<v8::ArrayBuffer> ab = input.As<v8::ArrayBuffer>();
        size_t byte_offset = 0;
        size_t length = ab->ByteLength();

        if (args.Length() > 1 && args[1]->IsNumber()) {
            byte_offset = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
        }
        if (args.Length() > 2 && args[2]->IsNumber()) {
            length = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
        }

        if (byte_offset + length > ab->ByteLength()) {
            p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Attempt to access memory outside buffer bounds")));
            return;
        }

        v8::Local<v8::Uint8Array> ui = v8::Uint8Array::New(ab, byte_offset, length);
        // Set the prototype to Buffer.prototype
        v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
        if (buffer_val->IsFunction()) {
            v8::Local<v8::Value> proto = buffer_val.As<v8::Function>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
            ui->SetPrototypeV2(context, proto).Check();
        }
        args.GetReturnValue().Set(ui);
        return;
    }

    // Case 3: TypedArray or Buffer
    if (input->IsUint8Array()) {
        v8::Local<v8::Uint8Array> src = input.As<v8::Uint8Array>();
        size_t len = src->ByteLength();
        v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, len);
        uint8_t* p_src = static_cast<uint8_t*>(src->Buffer()->GetBackingStore()->Data()) + src->ByteOffset();
        memcpy(ui->Buffer()->GetBackingStore()->Data(), p_src, len);
        args.GetReturnValue().Set(ui);
        return;
    }

    // Case 4: Array-like
    if (input->IsArray()) {
        v8::Local<v8::Array> arr = input.As<v8::Array>();
        uint32_t len = arr->Length();
        v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, len);
        uint8_t* p_data = static_cast<uint8_t*>(ui->Buffer()->GetBackingStore()->Data());
        for (uint32_t i = 0; i < len; i++) {
            p_data[i] = static_cast<uint8_t>(arr->Get(context, i).ToLocalChecked()->Uint32Value(context).FromMaybe(0));
        }
        args.GetReturnValue().Set(ui);
        return;
    }

    // Fallback: Try converting to string (for Objects with valueOf/toString)
    v8::Local<v8::String> s;
    if (input->ToString(context).ToLocal(&s)) {
        v8::String::Utf8Value str_val(p_isolate, s);
        v8::Local<v8::Uint8Array> ui = createBuffer(p_isolate, str_val.length());
        memcpy(ui->Buffer()->GetBackingStore()->Data(), *str_val, str_val.length());
        args.GetReturnValue().Set(ui);
        return;
    }

    p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The first argument must be one of type string, Buffer, ArrayBuffer, Array, or Array-like object.")));
}


void Buffer::concat(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsArray()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"list\" argument must be an instance of Array")));
        return;
    }

    v8::Local<v8::Array> list = args[0].As<v8::Array>();
    uint32_t list_len = list->Length();
    
    size_t total_length = 0;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        total_length = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    } else {
        for (uint32_t i = 0; i < list_len; i++) {
            v8::Local<v8::Value> item = list->Get(context, i).ToLocalChecked();
            if (item->IsUint8Array()) {
                total_length += item.As<v8::Uint8Array>()->ByteLength();
            }
        }
    }

    v8::Local<v8::Uint8Array> result = createBuffer(p_isolate, total_length);
    uint8_t* p_dst_data = static_cast<uint8_t*>(result->Buffer()->GetBackingStore()->Data());
    
    size_t offset = 0;
    for (uint32_t i = 0; i < list_len && offset < total_length; i++) {
        v8::Local<v8::Value> item = list->Get(context, i).ToLocalChecked();
        if (item->IsUint8Array()) {
            v8::Local<v8::Uint8Array> src = item.As<v8::Uint8Array>();
            size_t to_copy = std::min(src->ByteLength(), total_length - offset);
            void* p_src_data = src->Buffer()->GetBackingStore()->Data();
            memcpy(p_dst_data + offset, (uint8_t*)p_src_data + src->ByteOffset(), to_copy);
            offset += to_copy;
        }
    }

    args.GetReturnValue().Set(result);
}

void Buffer::isBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }
    
    v8::Local<v8::Value> val = args[0];
    if (!val->IsUint8Array()) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }

    // Check if prototype is Buffer.prototype
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
    if (buffer_val->IsFunction()) {
        v8::Local<v8::Value> buffer_proto = buffer_val.As<v8::Function>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
        v8::Local<v8::Value> obj_proto = val.As<v8::Object>()->GetPrototypeV2();
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, obj_proto->StrictEquals(buffer_proto)));
        return;
    }

    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
}

void Buffer::toString(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    
    if (!self->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Method toString called on incompatible receiver")));
        return;
    }

    v8::Local<v8::Uint8Array> ui = self.As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(ui->Buffer()->GetBackingStore()->Data());
    size_t total_len = ui->ByteLength();
    size_t offset    = ui->ByteOffset();

    // Parse optional start/end args
    int64_t start = 0;
    int64_t end   = static_cast<int64_t>(total_len);
    if (args.Length() > 1 && args[1]->IsNumber()) {
        start = args[1]->IntegerValue(p_isolate->GetCurrentContext()).FromMaybe(0);
        if (start < 0) start = std::max(static_cast<int64_t>(0), static_cast<int64_t>(total_len) + start);
        if (start > static_cast<int64_t>(total_len)) start = static_cast<int64_t>(total_len);
    }
    if (args.Length() > 2 && args[2]->IsNumber()) {
        end = args[2]->IntegerValue(p_isolate->GetCurrentContext()).FromMaybe(static_cast<int64_t>(total_len));
        if (end < 0) end = std::max(static_cast<int64_t>(0), static_cast<int64_t>(total_len) + end);
        if (end > static_cast<int64_t>(total_len)) end = static_cast<int64_t>(total_len);
    }
    if (end < start) end = start;
    size_t len = static_cast<size_t>(end - start);

    std::string encoding = "utf8";
    if (args.Length() > 0 && args[0]->IsString()) {
        v8::String::Utf8Value enc_str(p_isolate, args[0]);
        encoding = *enc_str;
    }

    const uint8_t* p_slice = p_data + offset + static_cast<size_t>(start);

    if (encoding == "hex") {
        std::string hex = bytesToHex(p_slice, len);
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, hex.c_str()).ToLocalChecked());
        return;
    } else if (encoding == "base64" || encoding == "base64url") {
        std::string b64 = bytesToBase64(p_slice, len);
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, b64.c_str()).ToLocalChecked());
        return;
    } else if (encoding == "latin1" || encoding == "binary" || encoding == "ascii") {
        // latin1: each byte maps directly to unicode codepoint 0-255
        std::string s(reinterpret_cast<const char*>(p_slice), len);
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, s.c_str(), v8::NewStringType::kNormal, static_cast<int32_t>(len)).ToLocalChecked());
        return;
    }

    // Default to utf8
    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, reinterpret_cast<const char*>(p_slice), v8::NewStringType::kNormal, static_cast<int32_t>(len)).ToLocalChecked());
}

void Buffer::write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString()) {
        args.GetReturnValue().Set(0);
        return;
    }

    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    size_t buf_length = self->ByteLength();

    size_t offset = 0;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        offset = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    }

    if (offset >= buf_length) {
        args.GetReturnValue().Set(0);
        return;
    }

    // Optional length param (3rd arg)
    size_t max_write = buf_length - offset;
    if (args.Length() > 2 && args[2]->IsNumber()) {
        size_t len_param = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
        if (len_param < max_write) max_write = len_param;
    }

    // Encoding is 3rd or 4th argument
    std::string encoding = "utf8";
    if (args.Length() > 3 && args[3]->IsString()) {
        v8::String::Utf8Value enc_str(p_isolate, args[3]);
        encoding = *enc_str;
    } else if (args.Length() > 2 && args[2]->IsString()) {
        v8::String::Utf8Value enc_str(p_isolate, args[2]);
        encoding = *enc_str;
    }

    v8::String::Utf8Value str(p_isolate, args[0]);
    size_t to_write = std::min(static_cast<size_t>(str.length()), max_write);

    if (encoding == "hex") {
        std::string hex_str(*str);
        size_t hex_len = hex_str.length() / 2;
        to_write = std::min(hex_len, max_write);
        for (size_t i = 0; i < to_write; i++) {
            int32_t high = hexValue(hex_str[i * 2]);
            int32_t low  = hexValue(hex_str[i * 2 + 1]);
            if (high == -1 || low == -1) { to_write = i; break; }
            p_data[offset + i] = static_cast<uint8_t>((high << 4) | low);
        }
    } else if (encoding == "base64" || encoding == "base64url") {
        std::vector<uint8_t> bytes = base64ToBytes(*str);
        to_write = std::min(bytes.size(), max_write);
        memcpy(p_data + offset, bytes.data(), to_write);
    } else if (encoding == "latin1" || encoding == "binary" || encoding == "ascii") {
        to_write = std::min(static_cast<size_t>(str.length()), max_write);
        memcpy(p_data + offset, *str, to_write);
    } else {
        memcpy(p_data + offset, *str, to_write);
    }

    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(to_write)));
}

void Buffer::fill(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    size_t length = self->ByteLength();

    size_t fill_offset = 0;
    size_t fill_end    = length;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        fill_offset = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
        if (fill_offset > length) fill_offset = length;
    }
    if (args.Length() > 2 && args[2]->IsNumber()) {
        fill_end = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(static_cast<int64_t>(length)));
        if (fill_end > length) fill_end = length;
    }
    if (fill_end < fill_offset) fill_end = fill_offset;
    size_t fill_len = fill_end - fill_offset;
    uint8_t* p_target = p_data + fill_offset;

    if (args.Length() < 1 || fill_len == 0) {
        memset(p_target, 0, fill_len);
    } else if (args[0]->IsNumber()) {
        uint8_t val = static_cast<uint8_t>(args[0]->Uint32Value(context).FromMaybe(0));
        memset(p_target, val, fill_len);
    } else if (args[0]->IsString()) {
        // Encoding can be 3rd or 4th argument
        std::string enc = "utf8";
        if (args.Length() > 3 && args[3]->IsString()) {
            v8::String::Utf8Value e(p_isolate, args[3]); enc = *e;
        } else if (args.Length() > 2 && args[2]->IsString()) {
            v8::String::Utf8Value e(p_isolate, args[2]); enc = *e;
        }
        v8::String::Utf8Value str_val(p_isolate, args[0]);
        size_t src_len = static_cast<size_t>(str_val.length());
        if (src_len == 0) {
            memset(p_target, 0, fill_len);
        } else {
            for (size_t i = 0; i < fill_len; ) {
                size_t to_copy = std::min(src_len, fill_len - i);
                memcpy(p_target + i, *str_val, to_copy);
                i += to_copy;
            }
        }
    } else if (args[0]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> fill_buf = args[0].As<v8::Uint8Array>();
        size_t src_len = fill_buf->ByteLength();
        if (src_len == 0) {
            memset(p_target, 0, fill_len);
        } else {
            uint8_t* p_src = static_cast<uint8_t*>(fill_buf->Buffer()->GetBackingStore()->Data()) + fill_buf->ByteOffset();
            for (size_t i = 0; i < fill_len; ) {
                size_t to_copy = std::min(src_len, fill_len - i);
                memcpy(p_target + i, p_src, to_copy);
                i += to_copy;
            }
        }
    }
    args.GetReturnValue().Set(args.This());
}

void Buffer::copy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"target\" argument must be an instance of Uint8Array")));
        return;
    }

    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    v8::Local<v8::Uint8Array> target = args[0].As<v8::Uint8Array>();

    size_t target_start = 0;
    size_t source_start = 0;
    size_t source_end = self->ByteLength();

    if (args.Length() > 1 && args[1]->IsNumber()) {
        target_start = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    }
    if (args.Length() > 2 && args[2]->IsNumber()) {
        source_start = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
    }
    if (args.Length() > 3 && args[3]->IsNumber()) {
        source_end = static_cast<size_t>(args[3]->IntegerValue(context).FromMaybe(static_cast<int64_t>(source_end)));
    }

    if (source_start >= self->ByteLength() || target_start >= target->ByteLength()) {
        args.GetReturnValue().Set(v8::Integer::New(p_isolate, 0));
        return;
    }

    size_t length = std::min(source_end - source_start, self->ByteLength() - source_start);
    length = std::min(length, target->ByteLength() - target_start);

    if (length > 0) {
        uint8_t* p_src = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset() + source_start;
        uint8_t* p_dst = static_cast<uint8_t*>(target->Buffer()->GetBackingStore()->Data()) + target->ByteOffset() + target_start;
        memmove(p_dst, p_src, length);
    }

    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(length)));
}

void Buffer::slice(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    if (!self->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Method slice called on incompatible receiver")));
        return;
    }

    v8::Local<v8::Uint8Array> ui = self.As<v8::Uint8Array>();
    int64_t start = 0;
    int64_t end = ui->ByteLength();

    if (args.Length() > 0 && args[0]->IsNumber()) {
        start = args[0]->IntegerValue(context).FromMaybe(0);
        if (start < 0) start += ui->ByteLength();
        if (start < 0) start = 0;
    }

    if (args.Length() > 1 && args[1]->IsNumber()) {
        end = args[1]->IntegerValue(context).FromMaybe(ui->ByteLength());
        if (end < 0) end += ui->ByteLength();
        if (end > (int64_t)ui->ByteLength()) end = ui->ByteLength();
    }

    if (end < start) end = start;

    size_t length = end - start;
    v8::Local<v8::Uint8Array> result = v8::Uint8Array::New(ui->Buffer(), ui->ByteOffset() + start, length);
    
    // Re-prototype it
    v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
    if (buffer_val->IsFunction()) {
        v8::Local<v8::Value> proto = buffer_val.As<v8::Function>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "prototype")).ToLocalChecked();
        result->SetPrototypeV2(context, proto).Check();
    }

    args.GetReturnValue().Set(result);
}

// ---- New static methods ----

void Buffer::allocUnsafeSlow(const v8::FunctionCallbackInfo<v8::Value>& args) {
    alloc(args);
}

void Buffer::isEncoding(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsString()) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }
    v8::String::Utf8Value enc(p_isolate, args[0]);
    std::string s(*enc);
    bool valid = (s == "utf8" || s == "utf-8" || s == "ascii" || s == "latin1"
               || s == "binary" || s == "base64" || s == "base64url"
               || s == "hex" || s == "ucs2" || s == "ucs-2"
               || s == "utf16le" || s == "utf-16le");
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, valid));
}

void Buffer::byteLength(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1) {
        args.GetReturnValue().Set(v8::Integer::New(p_isolate, 0));
        return;
    }
    if (args[0]->IsString()) {
        std::string enc = "utf8";
        if (args.Length() > 1 && args[1]->IsString()) {
            v8::String::Utf8Value e(p_isolate, args[1]);
            enc = *e;
        }
        if (enc == "hex") {
            v8::String::Utf8Value str(p_isolate, args[0]);
            args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(str.length() / 2)));
            return;
        }
        if (enc == "base64" || enc == "base64url") {
            v8::String::Utf8Value str(p_isolate, args[0]);
            int32_t len = static_cast<int32_t>(str.length());
            int32_t decoded = (len * 3) / 4;
            if (len > 0 && (*str)[len - 1] == '=') decoded--;
            if (len > 1 && (*str)[len - 2] == '=') decoded--;
            args.GetReturnValue().Set(v8::Integer::New(p_isolate, decoded));
            return;
        }
        v8::String::Utf8Value str(p_isolate, args[0]);
        args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(str.length())));
        return;
    }
    if (args[0]->IsArrayBuffer()) {
        args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(args[0].As<v8::ArrayBuffer>()->ByteLength())));
        return;
    }
    if (args[0]->IsArrayBufferView()) {
        args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(args[0].As<v8::ArrayBufferView>()->ByteLength())));
        return;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, 0));
}

void Buffer::compare(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsUint8Array() || !args[1]->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Arguments must be Buffers or Uint8Arrays")));
        return;
    }
    v8::Local<v8::Uint8Array> a = args[0].As<v8::Uint8Array>();
    v8::Local<v8::Uint8Array> b = args[1].As<v8::Uint8Array>();
    uint8_t* p_a = static_cast<uint8_t*>(a->Buffer()->GetBackingStore()->Data()) + a->ByteOffset();
    uint8_t* p_b = static_cast<uint8_t*>(b->Buffer()->GetBackingStore()->Data()) + b->ByteOffset();
    size_t len = std::min(a->ByteLength(), b->ByteLength());
    int32_t cmp = len > 0 ? memcmp(p_a, p_b, len) : 0;
    if (cmp == 0) {
        if (a->ByteLength() < b->ByteLength()) cmp = -1;
        else if (a->ByteLength() > b->ByteLength()) cmp = 1;
    } else {
        cmp = cmp < 0 ? -1 : 1;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, cmp));
}

// ---- New instance methods ----

void Buffer::subarray(const v8::FunctionCallbackInfo<v8::Value>& args) {
    slice(args);
}

void Buffer::equals(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Argument must be a Buffer or Uint8Array")));
        return;
    }
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    v8::Local<v8::Uint8Array> other = args[0].As<v8::Uint8Array>();
    if (self->ByteLength() != other->ByteLength()) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }
    uint8_t* p_self = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    uint8_t* p_other = static_cast<uint8_t*>(other->Buffer()->GetBackingStore()->Data()) + other->ByteOffset();
    bool eq = (memcmp(p_self, p_other, self->ByteLength()) == 0);
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, eq));
}

void Buffer::compare_instance(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Argument must be a Buffer or Uint8Array")));
        return;
    }
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    v8::Local<v8::Uint8Array> target = args[0].As<v8::Uint8Array>();
    size_t target_start = 0, target_end = target->ByteLength();
    size_t source_start = 0, source_end = self->ByteLength();
    if (args.Length() > 1 && args[1]->IsNumber()) target_start = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    if (args.Length() > 2 && args[2]->IsNumber()) target_end   = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(static_cast<int64_t>(target_end)));
    if (args.Length() > 3 && args[3]->IsNumber()) source_start = static_cast<size_t>(args[3]->IntegerValue(context).FromMaybe(0));
    if (args.Length() > 4 && args[4]->IsNumber()) source_end   = static_cast<size_t>(args[4]->IntegerValue(context).FromMaybe(static_cast<int64_t>(source_end)));
    uint8_t* p_self   = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data())   + self->ByteOffset()   + source_start;
    uint8_t* p_target = static_cast<uint8_t*>(target->Buffer()->GetBackingStore()->Data()) + target->ByteOffset() + target_start;
    size_t src_len = source_end > source_start ? source_end - source_start : 0;
    size_t tgt_len = target_end > target_start ? target_end - target_start : 0;
    size_t len = std::min(src_len, tgt_len);
    int32_t cmp = len > 0 ? memcmp(p_self, p_target, len) : 0;
    if (cmp == 0) {
        if (src_len < tgt_len) cmp = -1;
        else if (src_len > tgt_len) cmp = 1;
    } else {
        cmp = cmp < 0 ? -1 : 1;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, cmp));
}

static int64_t bufferIndexOf(uint8_t* p_haystack, size_t hay_len, uint8_t* p_needle, size_t needle_len, int64_t byte_offset, bool reverse) {
    if (needle_len == 0) return reverse ? static_cast<int64_t>(hay_len) : byte_offset;
    if (needle_len > hay_len) return -1;
    if (!reverse) {
        for (int64_t i = byte_offset; i <= static_cast<int64_t>(hay_len - needle_len); i++) {
            if (memcmp(p_haystack + i, p_needle, needle_len) == 0) return i;
        }
    } else {
        int64_t start = static_cast<int64_t>(hay_len - needle_len);
        if (byte_offset < start) start = byte_offset;
        for (int64_t i = start; i >= 0; i--) {
            if (memcmp(p_haystack + i, p_needle, needle_len) == 0) return i;
        }
    }
    return -1;
}

void Buffer::indexOf(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    size_t buf_len = self->ByteLength();
    int64_t byte_offset = 0;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        byte_offset = args[1]->IntegerValue(context).FromMaybe(0);
        if (byte_offset < 0) byte_offset = std::max(static_cast<int64_t>(0), static_cast<int64_t>(buf_len) + byte_offset);
    }
    int64_t result = -1;
    if (args.Length() >= 1) {
        if (args[0]->IsNumber()) {
            uint8_t val = static_cast<uint8_t>(args[0]->Uint32Value(context).FromMaybe(0));
            for (int64_t i = byte_offset; i < static_cast<int64_t>(buf_len); i++) {
                if (p_data[i] == val) { result = i; break; }
            }
        } else if (args[0]->IsString()) {
            v8::String::Utf8Value str(p_isolate, args[0]);
            result = bufferIndexOf(p_data, buf_len, reinterpret_cast<uint8_t*>(*str), str.length(), byte_offset, false);
        } else if (args[0]->IsUint8Array()) {
            v8::Local<v8::Uint8Array> needle = args[0].As<v8::Uint8Array>();
            uint8_t* p_needle = static_cast<uint8_t*>(needle->Buffer()->GetBackingStore()->Data()) + needle->ByteOffset();
            result = bufferIndexOf(p_data, buf_len, p_needle, needle->ByteLength(), byte_offset, false);
        }
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(result)));
}

void Buffer::lastIndexOf(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    size_t buf_len = self->ByteLength();
    int64_t byte_offset = static_cast<int64_t>(buf_len) - 1;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        byte_offset = args[1]->IntegerValue(context).FromMaybe(0);
        if (byte_offset < 0) byte_offset = static_cast<int64_t>(buf_len) + byte_offset;
    }
    int64_t result = -1;
    if (args.Length() >= 1) {
        if (args[0]->IsNumber()) {
            uint8_t val = static_cast<uint8_t>(args[0]->Uint32Value(context).FromMaybe(0));
            for (int64_t i = std::min(byte_offset, static_cast<int64_t>(buf_len) - 1); i >= 0; i--) {
                if (p_data[i] == val) { result = i; break; }
            }
        } else if (args[0]->IsString()) {
            v8::String::Utf8Value str(p_isolate, args[0]);
            result = bufferIndexOf(p_data, buf_len, reinterpret_cast<uint8_t*>(*str), str.length(), byte_offset, true);
        } else if (args[0]->IsUint8Array()) {
            v8::Local<v8::Uint8Array> needle = args[0].As<v8::Uint8Array>();
            uint8_t* p_needle = static_cast<uint8_t*>(needle->Buffer()->GetBackingStore()->Data()) + needle->ByteOffset();
            result = bufferIndexOf(p_data, buf_len, p_needle, needle->ByteLength(), byte_offset, true);
        }
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(result)));
}

void Buffer::includes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    indexOf(args);
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Value> idx_val = args.GetReturnValue().Get();
    int32_t idx = idx_val->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(-1);
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, idx >= 0));
}

void Buffer::toJSON(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    size_t len = self->ByteLength();
    v8::Local<v8::Array> data_arr = v8::Array::New(p_isolate, static_cast<int32_t>(len));
    for (size_t i = 0; i < len; i++) {
        data_arr->Set(context, static_cast<uint32_t>(i), v8::Integer::New(p_isolate, p_data[i])).Check();
    }
    v8::Local<v8::Object> obj = v8::Object::New(p_isolate);
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "type"), v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "data"), data_arr).Check();
    args.GetReturnValue().Set(obj);
}

void Buffer::swap16(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    size_t len = self->ByteLength();
    if (len % 2 != 0) {
        p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Buffer size must be a multiple of 16-bits")));
        return;
    }
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    for (size_t i = 0; i < len; i += 2) {
        uint8_t tmp = p_data[i]; p_data[i] = p_data[i + 1]; p_data[i + 1] = tmp;
    }
    args.GetReturnValue().Set(args.This());
}

void Buffer::swap32(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    size_t len = self->ByteLength();
    if (len % 4 != 0) {
        p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Buffer size must be a multiple of 32-bits")));
        return;
    }
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    for (size_t i = 0; i < len; i += 4) {
        uint8_t a = p_data[i], b = p_data[i+1], c = p_data[i+2], d = p_data[i+3];
        p_data[i] = d; p_data[i+1] = c; p_data[i+2] = b; p_data[i+3] = a;
    }
    args.GetReturnValue().Set(args.This());
}

void Buffer::swap64(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    size_t len = self->ByteLength();
    if (len % 8 != 0) {
        p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Buffer size must be a multiple of 64-bits")));
        return;
    }
    uint8_t* p_data = static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
    for (size_t i = 0; i < len; i += 8) {
        for (size_t j = 0; j < 4; j++) {
            uint8_t tmp = p_data[i + j]; p_data[i + j] = p_data[i + 7 - j]; p_data[i + 7 - j] = tmp;
        }
    }
    args.GetReturnValue().Set(args.This());
}

// ---- Read numeric helpers ----
static uint8_t* getBufData(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Uint8Array> self = args.This().As<v8::Uint8Array>();
    return static_cast<uint8_t*>(self->Buffer()->GetBackingStore()->Data()) + self->ByteOffset();
}
static size_t getOffset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() > 0 && args[0]->IsNumber()) {
        return static_cast<size_t>(args[0]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    }
    return 0;
}

#define CHECK_OFFSET(off, n, len) do { \
    if ((off) + (n) > (len)) { \
        args.GetIsolate()->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(args.GetIsolate(), "Attempt to access memory outside buffer bounds"))); \
        return; \
    } \
} while (0)

void Buffer::readUInt8(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 1, len);
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(args.GetIsolate(), p_data[off]));
}
void Buffer::readInt8(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 1, len);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int8_t>(p_data[off])));
}
void Buffer::readUInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 2, len);
    uint32_t val = (static_cast<uint32_t>(p_data[off]) << 8) | p_data[off+1];
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(args.GetIsolate(), val));
}
void Buffer::readUInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 2, len);
    uint32_t val = (static_cast<uint32_t>(p_data[off+1]) << 8) | p_data[off];
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(args.GetIsolate(), val));
}
void Buffer::readInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 2, len);
    int16_t val = static_cast<int16_t>((static_cast<uint32_t>(p_data[off]) << 8) | p_data[off+1]);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), val));
}
void Buffer::readInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 2, len);
    int16_t val = static_cast<int16_t>((static_cast<uint32_t>(p_data[off+1]) << 8) | p_data[off]);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), val));
}
void Buffer::readUInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    uint32_t val = (static_cast<uint32_t>(p_data[off])<<24)|(static_cast<uint32_t>(p_data[off+1])<<16)|(static_cast<uint32_t>(p_data[off+2])<<8)|p_data[off+3];
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(args.GetIsolate(), val));
}
void Buffer::readUInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    uint32_t val = (static_cast<uint32_t>(p_data[off+3])<<24)|(static_cast<uint32_t>(p_data[off+2])<<16)|(static_cast<uint32_t>(p_data[off+1])<<8)|p_data[off];
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(args.GetIsolate(), val));
}
void Buffer::readInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    int32_t val = static_cast<int32_t>((static_cast<uint32_t>(p_data[off])<<24)|(static_cast<uint32_t>(p_data[off+1])<<16)|(static_cast<uint32_t>(p_data[off+2])<<8)|p_data[off+3]);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), val));
}
void Buffer::readInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    int32_t val = static_cast<int32_t>((static_cast<uint32_t>(p_data[off+3])<<24)|(static_cast<uint32_t>(p_data[off+2])<<16)|(static_cast<uint32_t>(p_data[off+1])<<8)|p_data[off]);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), val));
}
void Buffer::readFloatBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    uint32_t raw = (static_cast<uint32_t>(p_data[off])<<24)|(static_cast<uint32_t>(p_data[off+1])<<16)|(static_cast<uint32_t>(p_data[off+2])<<8)|p_data[off+3];
    float val; memcpy(&val, &raw, 4);
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), static_cast<double>(val)));
}
void Buffer::readFloatLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 4, len);
    float val; memcpy(&val, p_data + off, 4);
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), static_cast<double>(val)));
}
void Buffer::readDoubleBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    uint8_t tmp[8];
    for (int32_t i = 0; i < 8; i++) tmp[i] = p_data[off + 7 - i];
    double val; memcpy(&val, tmp, 8);
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), val));
}
void Buffer::readDoubleLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    double val; memcpy(&val, p_data + off, 8);
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), val));
}
void Buffer::readBigInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    int64_t val = 0;
    for (int32_t i = 0; i < 8; i++) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::BigInt::New(p_isolate, val));
}
void Buffer::readBigInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    int64_t val = 0;
    for (int32_t i = 7; i >= 0; i--) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::BigInt::New(p_isolate, val));
}
void Buffer::readBigUInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    uint64_t val = 0;
    for (int32_t i = 0; i < 8; i++) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(p_isolate, val));
}
void Buffer::readBigUInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data = getBufData(args); size_t off = getOffset(args);
    size_t len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, 8, len);
    uint64_t val = 0;
    for (int32_t i = 7; i >= 0; i--) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(p_isolate, val));
}

// ---- Write numeric methods ----
#define GET_WRITE_OFFSET(args, n) \
    size_t off = 0; \
    if (args.Length() > 1 && args[1]->IsNumber()) off = static_cast<size_t>(args[1]->IntegerValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0)); \
    size_t buf_len_w = args.This().As<v8::Uint8Array>()->ByteLength(); \
    CHECK_OFFSET(off, n, buf_len_w); \
    uint8_t* p_w = getBufData(args);

void Buffer::writeUInt8(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 1);
    p_w[off] = static_cast<uint8_t>(args[0]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 1)));
}
void Buffer::writeInt8(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 1);
    p_w[off] = static_cast<uint8_t>(static_cast<int8_t>(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0)));
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 1)));
}
void Buffer::writeUInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 2);
    uint32_t val = args[0]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    p_w[off] = static_cast<uint8_t>(val >> 8); p_w[off+1] = static_cast<uint8_t>(val);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 2)));
}
void Buffer::writeUInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 2);
    uint32_t val = args[0]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    p_w[off] = static_cast<uint8_t>(val); p_w[off+1] = static_cast<uint8_t>(val >> 8);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 2)));
}
void Buffer::writeInt16BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 2);
    uint32_t val = static_cast<uint32_t>(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    p_w[off] = static_cast<uint8_t>(val >> 8); p_w[off+1] = static_cast<uint8_t>(val);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 2)));
}
void Buffer::writeInt16LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 2);
    uint32_t val = static_cast<uint32_t>(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    p_w[off] = static_cast<uint8_t>(val); p_w[off+1] = static_cast<uint8_t>(val >> 8);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 2)));
}
void Buffer::writeUInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    uint32_t val = args[0]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    p_w[off]=static_cast<uint8_t>(val>>24); p_w[off+1]=static_cast<uint8_t>(val>>16);
    p_w[off+2]=static_cast<uint8_t>(val>>8); p_w[off+3]=static_cast<uint8_t>(val);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeUInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    uint32_t val = args[0]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    p_w[off]=static_cast<uint8_t>(val); p_w[off+1]=static_cast<uint8_t>(val>>8);
    p_w[off+2]=static_cast<uint8_t>(val>>16); p_w[off+3]=static_cast<uint8_t>(val>>24);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeInt32BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    uint32_t val = static_cast<uint32_t>(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    p_w[off]=static_cast<uint8_t>(val>>24); p_w[off+1]=static_cast<uint8_t>(val>>16);
    p_w[off+2]=static_cast<uint8_t>(val>>8); p_w[off+3]=static_cast<uint8_t>(val);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeInt32LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    uint32_t val = static_cast<uint32_t>(args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    p_w[off]=static_cast<uint8_t>(val); p_w[off+1]=static_cast<uint8_t>(val>>8);
    p_w[off+2]=static_cast<uint8_t>(val>>16); p_w[off+3]=static_cast<uint8_t>(val>>24);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeFloatBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    float val = static_cast<float>(args[0]->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0.0));
    uint32_t raw; memcpy(&raw, &val, 4);
    p_w[off]=static_cast<uint8_t>(raw>>24); p_w[off+1]=static_cast<uint8_t>(raw>>16);
    p_w[off+2]=static_cast<uint8_t>(raw>>8); p_w[off+3]=static_cast<uint8_t>(raw);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeFloatLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 4);
    float val = static_cast<float>(args[0]->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0.0));
    memcpy(p_w + off, &val, 4);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 4)));
}
void Buffer::writeDoubleBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 8);
    double val = args[0]->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0.0);
    uint8_t tmp[8]; memcpy(tmp, &val, 8);
    for (int32_t i = 0; i < 8; i++) p_w[off + i] = tmp[7 - i];
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}
void Buffer::writeDoubleLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1) return;
    GET_WRITE_OFFSET(args, 8);
    double val = args[0]->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0.0);
    memcpy(p_w + off, &val, 8);
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}
void Buffer::writeBigInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsBigInt()) return;
    GET_WRITE_OFFSET(args, 8);
    int64_t val = args[0].As<v8::BigInt>()->Int64Value();
    for (int32_t i = 7; i >= 0; i--) { p_w[off + i] = static_cast<uint8_t>(val & 0xff); val >>= 8; }
    // fix order for BE
    uint8_t tmp[8]; memcpy(tmp, p_w + off, 8);
    for (int32_t i = 0; i < 8; i++) p_w[off + i] = tmp[7 - i];
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}
void Buffer::writeBigInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsBigInt()) return;
    GET_WRITE_OFFSET(args, 8);
    int64_t val = args[0].As<v8::BigInt>()->Int64Value();
    for (int32_t i = 0; i < 8; i++) { p_w[off + i] = static_cast<uint8_t>(val & 0xff); val >>= 8; }
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}
void Buffer::writeBigUInt64BE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsBigInt()) return;
    GET_WRITE_OFFSET(args, 8);
    uint64_t val = args[0].As<v8::BigInt>()->Uint64Value();
    for (int32_t i = 0; i < 8; i++) { p_w[off + i] = static_cast<uint8_t>((val >> (56 - i*8)) & 0xff); }
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}
void Buffer::writeBigUInt64LE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsBigInt()) return;
    GET_WRITE_OFFSET(args, 8);
    uint64_t val = args[0].As<v8::BigInt>()->Uint64Value();
    for (int32_t i = 0; i < 8; i++) { p_w[off + i] = static_cast<uint8_t>(val & 0xff); val >>= 8; }
    args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), static_cast<int32_t>(off + 8)));
}

// ---- Variable-length read methods ----
void Buffer::readIntBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2) { p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Missing offset or byteLength"))); return; }
    size_t off = static_cast<size_t>(args[0]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    int64_t val = 0;
    for (size_t i = 0; i < byte_local_len; i++) val = (val << 8) | p_data[off + i];
    // Sign extend
    int64_t bits = static_cast<int64_t>(byte_local_len) * 8;
    int64_t shift = 64 - bits;
    val = (val << shift) >> shift;
    args.GetReturnValue().Set(v8::Number::New(p_isolate, static_cast<double>(val)));
}
void Buffer::readIntLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2) { p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Missing offset or byteLength"))); return; }
    size_t off = static_cast<size_t>(args[0]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    int64_t val = 0;
    for (int32_t i = static_cast<int32_t>(byte_local_len) - 1; i >= 0; i--) val = (val << 8) | p_data[off + i];
    int64_t bits = static_cast<int64_t>(byte_local_len) * 8;
    int64_t shift = 64 - bits;
    val = (val << shift) >> shift;
    args.GetReturnValue().Set(v8::Number::New(p_isolate, static_cast<double>(val)));
}
void Buffer::readUIntBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2) { p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Missing offset or byteLength"))); return; }
    size_t off = static_cast<size_t>(args[0]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    uint64_t val = 0;
    for (size_t i = 0; i < byte_local_len; i++) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::Number::New(p_isolate, static_cast<double>(val)));
}
void Buffer::readUIntLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2) { p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "Missing offset or byteLength"))); return; }
    size_t off = static_cast<size_t>(args[0]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    uint64_t val = 0;
    for (int32_t i = static_cast<int32_t>(byte_local_len) - 1; i >= 0; i--) val = (val << 8) | p_data[off + i];
    args.GetReturnValue().Set(v8::Number::New(p_isolate, static_cast<double>(val)));
}

// ---- Write variable-length methods ----
void Buffer::writeIntBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 3) return;
    int64_t write_val = static_cast<int64_t>(args[0]->NumberValue(context).FromMaybe(0));
    size_t off = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    for (int32_t i = static_cast<int32_t>(byte_local_len) - 1; i >= 0; i--) {
        p_data[off + i] = static_cast<uint8_t>(write_val & 0xff);
        write_val >>= 8;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(off + byte_local_len)));
}
void Buffer::writeIntLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 3) return;
    int64_t write_val = static_cast<int64_t>(args[0]->NumberValue(context).FromMaybe(0));
    size_t off = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    for (size_t i = 0; i < byte_local_len; i++) {
        p_data[off + i] = static_cast<uint8_t>(write_val & 0xff);
        write_val >>= 8;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(off + byte_local_len)));
}
void Buffer::writeUIntBE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 3) return;
    uint64_t write_val = static_cast<uint64_t>(args[0]->NumberValue(context).FromMaybe(0));
    size_t off = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    for (int32_t i = static_cast<int32_t>(byte_local_len) - 1; i >= 0; i--) {
        p_data[off + i] = static_cast<uint8_t>(write_val & 0xff);
        write_val >>= 8;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(off + byte_local_len)));
}
void Buffer::writeUIntLE(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 3) return;
    uint64_t write_val = static_cast<uint64_t>(args[0]->NumberValue(context).FromMaybe(0));
    size_t off = static_cast<size_t>(args[1]->IntegerValue(context).FromMaybe(0));
    size_t byte_local_len = static_cast<size_t>(args[2]->IntegerValue(context).FromMaybe(0));
    size_t buf_len = args.This().As<v8::Uint8Array>()->ByteLength();
    CHECK_OFFSET(off, byte_local_len, buf_len);
    uint8_t* p_data = getBufData(args);
    for (size_t i = 0; i < byte_local_len; i++) {
        p_data[off + i] = static_cast<uint8_t>(write_val & 0xff);
        write_val >>= 8;
    }
    args.GetReturnValue().Set(v8::Integer::New(p_isolate, static_cast<int32_t>(off + byte_local_len)));
}

void Buffer::atob(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1) return;
    v8::String::Utf8Value str(p_isolate, args[0]);
    std::vector<uint8_t> bytes = base64ToBytes(*str);
    // Latin1 string from bytes
    args.GetReturnValue().Set(v8::String::NewFromOneByte(p_isolate, bytes.data(), v8::NewStringType::kNormal, static_cast<int32_t>(bytes.size())).ToLocalChecked());
}

void Buffer::btoa(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1) return;
    v8::String::Utf8Value str(p_isolate, args[0]);
    std::string b64 = bytesToBase64(reinterpret_cast<const uint8_t*>(*str), str.length());
    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, b64.c_str()).ToLocalChecked());
}

void Buffer::isAscii(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsUint8Array()) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }
    v8::Local<v8::Uint8Array> ui = args[0].As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(ui->Buffer()->GetBackingStore()->Data()) + ui->ByteOffset();
    size_t len = ui->ByteLength();
    for (size_t i = 0; i < len; i++) {
        if (p_data[i] > 127) {
            args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
            return;
        }
    }
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, true));
}

static bool isValidUtf8(const uint8_t* p_data, size_t length) {
    size_t i = 0;
    while (i < length) {
        if (p_data[i] <= 0x7F) {
            i += 1;
        } else if ((p_data[i] & 0xE0) == 0xC0) {
            if (i + 1 >= length || (p_data[i+1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((p_data[i] & 0xF0) == 0xE0) {
            if (i + 2 >= length || (p_data[i+1] & 0xC0) != 0x80 || (p_data[i+2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((p_data[i] & 0xF8) == 0xF0) {
            if (i + 3 >= length || (p_data[i+1] & 0xC0) != 0x80 || (p_data[i+2] & 0xC0) != 0x80 || (p_data[i+3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

void Buffer::isUtf8(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsUint8Array()) {
        args.GetReturnValue().Set(v8::Boolean::New(p_isolate, false));
        return;
    }
    v8::Local<v8::Uint8Array> ui = args[0].As<v8::Uint8Array>();
    uint8_t* p_data = static_cast<uint8_t*>(ui->Buffer()->GetBackingStore()->Data()) + ui->ByteOffset();
    size_t len = ui->ByteLength();
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, isValidUtf8(p_data, len)));
}

} // namespace module
} // namespace zane



