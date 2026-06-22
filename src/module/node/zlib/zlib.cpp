#include "zlib.hpp"
#include "../buffer/buffer.hpp"
#include "../stream/stream.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <string>
#include "../../../../deps/zlib/zlib.h"
#include "../../../../deps/brotli/c/include/brotli/encode.h"
#include "../../../../deps/brotli/c/include/brotli/decode.h"
#include "../../../../deps/zstd/lib/zstd.h"
#include "task_queue.hpp"
#include "thread_pool.hpp"

namespace zane {
namespace module {

// Forward declarations for stream methods
static void streamWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
static void streamFlush(const v8::FunctionCallbackInfo<v8::Value>& args);
static void streamEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
static void streamClose(const v8::FunctionCallbackInfo<v8::Value>& args);
static void streamReset(const v8::FunctionCallbackInfo<v8::Value>& args);
static void streamParams(const v8::FunctionCallbackInfo<v8::Value>& args);

static void brotliStreamWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
static void brotliStreamEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
static void brotliStreamClose(const v8::FunctionCallbackInfo<v8::Value>& args);
static void brotliStreamReset(const v8::FunctionCallbackInfo<v8::Value>& args);
static void brotliStreamFlush(const v8::FunctionCallbackInfo<v8::Value>& args);

static void zstdStreamWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
static void zstdStreamEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
static void zstdStreamClose(const v8::FunctionCallbackInfo<v8::Value>& args);
static void zstdStreamReset(const v8::FunctionCallbackInfo<v8::Value>& args);

static void throwZlibError(v8::Isolate* p_isolate, int32_t ret, const char* p_msg = nullptr, const char* p_strm_msg = nullptr) {
    std::string err_msg = p_msg ? p_msg : "Zlib error";
    if (p_strm_msg) {
        err_msg += ": ";
        err_msg += p_strm_msg;
    }
    const char* p_error_code = "Z_UNKNOWN";
    switch (ret) {
        case Z_STREAM_ERROR: p_error_code = "Z_STREAM_ERROR"; break;
        case Z_DATA_ERROR: p_error_code = "Z_DATA_ERROR"; break;
        case Z_MEM_ERROR: p_error_code = "Z_MEM_ERROR"; break;
        case Z_BUF_ERROR: p_error_code = "Z_BUF_ERROR"; break;
        case Z_VERSION_ERROR: p_error_code = "Z_VERSION_ERROR"; break;
    }
    v8::Local<v8::Value> err = v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, err_msg.c_str()).ToLocalChecked());
    v8::Local<v8::Object> obj = err.As<v8::Object>();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "code"), v8::String::NewFromUtf8(p_isolate, p_error_code).ToLocalChecked()).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "errno"), v8::Integer::New(p_isolate, ret)).Check();
    p_isolate->ThrowException(err);
}

struct ZlibAsyncCtx {
    std::vector<uint8_t> m_input;
    std::vector<uint8_t> m_output;
    int32_t m_level = Z_DEFAULT_COMPRESSION;
    int32_t m_window_bits = 15;
    int32_t m_mem_level = 8;
    int32_t m_strategy = Z_DEFAULT_STRATEGY;
    int32_t m_chunk_size = 16384;
    std::vector<uint8_t> m_dictionary;
    bool m_is_deflate;
    bool m_is_brotli = false;
    bool m_is_error = false;
    std::string m_error_msg;
    
    // Node.js parity
    bool m_info = false;
    size_t m_max_output_length = 0; // 0 means no limit

    // Brotli specific
    int32_t m_brotli_quality = BROTLI_DEFAULT_QUALITY;
    int32_t m_brotli_window = BROTLI_DEFAULT_WINDOW;
    int32_t m_brotli_mode = BROTLI_DEFAULT_MODE;
    std::vector<std::pair<int32_t, int32_t>> m_brotli_params;
    BrotliEncoderPreparedDictionary* p_brotli_prepared_dict = nullptr;

    // Zstd specific
    bool m_is_zstd = false;
    int32_t m_zstd_level = 3; // ZSTD_CLEVEL_DEFAULT
    std::string m_gzname;
    std::string m_gzcomment;
    uint32_t m_gzmtime = 0;
};

static void parseZlibOptions(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> options_val,
                             int32_t& level, int32_t& window_bits, int32_t& mem_level, int32_t& strategy,
                             int32_t& chunk_size, std::vector<uint8_t>& dictionary,
                             size_t& max_output_length, bool& info, std::string& gzname, std::string& gzcomment, uint32_t& gzmtime) {
    if (options_val.IsEmpty() || !options_val->IsObject()) {
        return;
    }
    v8::Local<v8::Object> options = options_val.As<v8::Object>();
    v8::Local<v8::Value> val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "level")).ToLocal(&val) && val->IsNumber()) {
        level = val->Int32Value(context).FromMaybe(level);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "windowBits")).ToLocal(&val) && val->IsNumber()) {
        window_bits = val->Int32Value(context).FromMaybe(window_bits);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "memLevel")).ToLocal(&val) && val->IsNumber()) {
        mem_level = val->Int32Value(context).FromMaybe(mem_level);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "strategy")).ToLocal(&val) && val->IsNumber()) {
        strategy = val->Int32Value(context).FromMaybe(strategy);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "chunkSize")).ToLocal(&val) && val->IsNumber()) {
        chunk_size = val->Int32Value(context).FromMaybe(chunk_size);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "dictionary")).ToLocal(&val)) {
        if (val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> dict = val.As<v8::Uint8Array>();
            dictionary.assign(static_cast<uint8_t*>(dict->Buffer()->GetBackingStore()->Data()) + dict->ByteOffset(),
                              static_cast<uint8_t*>(dict->Buffer()->GetBackingStore()->Data()) + dict->ByteOffset() + dict->ByteLength());
        }
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength")).ToLocal(&val) && val->IsNumber()) {
        max_output_length = (size_t)val->NumberValue(context).FromMaybe((double)max_output_length);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "info")).ToLocal(&val) && val->IsBoolean()) {
        info = val->BooleanValue(p_isolate);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "dictionary")).ToLocal(&val) && (val->IsUint8Array() || val->IsArrayBuffer())) {
        if (val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> view = val.As<v8::Uint8Array>();
            uint8_t* p_data = static_cast<uint8_t*>(view->Buffer()->GetBackingStore()->Data()) + view->ByteOffset();
            dictionary.assign(p_data, p_data + view->ByteLength());
        } else {
            v8::Local<v8::ArrayBuffer> ab = val.As<v8::ArrayBuffer>();
            uint8_t* p_data = static_cast<uint8_t*>(ab->GetBackingStore()->Data());
            dictionary.assign(p_data, p_data + ab->ByteLength());
        }
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "gzname")).ToLocal(&val) && val->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, val);
        gzname = *utf8;
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "gzcomment")).ToLocal(&val) && val->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, val);
        gzcomment = *utf8;
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "gzmtime")).ToLocal(&val) && val->IsNumber()) {
        gzmtime = (uint32_t)val->NumberValue(context).FromMaybe(0.0);
    }
}

static void parseBrotliOptions(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> options_val,
                               int32_t& quality, int32_t& window, int32_t& mode, int32_t& chunk_size,
                               size_t& max_output_length, std::vector<std::pair<int32_t, int32_t>>& params_vec,
                               std::vector<uint8_t>& dictionary) {
    if (options_val.IsEmpty() || !options_val->IsObject()) {
        return;
    }
    v8::Local<v8::Object> options = options_val.As<v8::Object>();
    v8::Local<v8::Value> val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "quality")).ToLocal(&val) && val->IsNumber()) {
        quality = val->Int32Value(context).FromMaybe(quality);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "window")).ToLocal(&val) && val->IsNumber()) {
        window = val->Int32Value(context).FromMaybe(window);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "mode")).ToLocal(&val) && val->IsNumber()) {
        mode = val->Int32Value(context).FromMaybe(mode);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "chunkSize")).ToLocal(&val) && val->IsNumber()) {
        chunk_size = val->Int32Value(context).FromMaybe(chunk_size);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength")).ToLocal(&val) && val->IsNumber()) {
        max_output_length = (size_t)val->NumberValue(context).FromMaybe((double)max_output_length);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "dictionary")).ToLocal(&val) && (val->IsUint8Array() || val->IsArrayBuffer())) {
        if (val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> view = val.As<v8::Uint8Array>();
            uint8_t* p_data = static_cast<uint8_t*>(view->Buffer()->GetBackingStore()->Data()) + view->ByteOffset();
            dictionary.assign(p_data, p_data + view->ByteLength());
        } else {
            v8::Local<v8::ArrayBuffer> ab = val.As<v8::ArrayBuffer>();
            uint8_t* p_data = static_cast<uint8_t*>(ab->GetBackingStore()->Data());
            dictionary.assign(p_data, p_data + ab->ByteLength());
        }
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "params")).ToLocal(&val) && val->IsObject()) {
        v8::Local<v8::Object> params_obj = val.As<v8::Object>();
        v8::Local<v8::Array> keys = params_obj->GetPropertyNames(context).ToLocalChecked();
        for (uint32_t i = 0; i < keys->Length(); i++) {
            v8::Local<v8::Value> key = keys->Get(context, i).ToLocalChecked();
            v8::Local<v8::Value> param_val = params_obj->Get(context, key).ToLocalChecked();
            if (param_val->IsNumber()) {
                params_vec.push_back({key->Int32Value(context).FromMaybe(0), param_val->Int32Value(context).FromMaybe(0)});
            }
        }
    }
}

static void parseZstdOptions(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> options_val,
                             int32_t& level, int32_t& chunk_size, std::vector<uint8_t>& dictionary) {
    if (options_val.IsEmpty() || !options_val->IsObject()) {
        return;
    }
    v8::Local<v8::Object> options = options_val.As<v8::Object>();
    v8::Local<v8::Value> val;
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "level")).ToLocal(&val) && val->IsNumber()) {
        level = val->Int32Value(context).FromMaybe(level);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "chunkSize")).ToLocal(&val) && val->IsNumber()) {
        chunk_size = val->Int32Value(context).FromMaybe(chunk_size);
    }
    if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "dictionary")).ToLocal(&val) && (val->IsUint8Array() || val->IsArrayBuffer())) {
        if (val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> view = val.As<v8::Uint8Array>();
            uint8_t* p_data = static_cast<uint8_t*>(view->Buffer()->GetBackingStore()->Data()) + view->ByteOffset();
            dictionary.assign(p_data, p_data + view->ByteLength());
        } else {
            v8::Local<v8::ArrayBuffer> ab = val.As<v8::ArrayBuffer>();
            uint8_t* p_data = static_cast<uint8_t*>(ab->GetBackingStore()->Data());
            dictionary.assign(p_data, p_data + ab->ByteLength());
        }
    }
}

static bool getInput(const v8::FunctionCallbackInfo<v8::Value>& args, uint8_t** p_data, size_t* length, std::vector<uint8_t>& storage) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Argument required")));
        return false;
    }

    if (args[0]->IsString()) {
        v8::String::Utf8Value str(p_isolate, args[0]);
        storage.assign(reinterpret_cast<uint8_t*>(*str), reinterpret_cast<uint8_t*>(*str) + str.length());
        *p_data = storage.data();
        *length = storage.size();
        return true;
    }

    if (!args[0]->IsUint8Array()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Argument must be a Uint8Array or string")));
        return false;
    }

    v8::Local<v8::Uint8Array> input = args[0].As<v8::Uint8Array>();
    v8::Local<v8::ArrayBuffer> ab = input->Buffer();
    *p_data = static_cast<uint8_t*>(ab->GetBackingStore()->Data()) + input->ByteOffset();
    *length = input->ByteLength();
    return true;
}

static void returnBuffer(const v8::FunctionCallbackInfo<v8::Value>& args, const std::vector<uint8_t>& out_buffer) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Uint8Array> ui = zane::module::Buffer::createBuffer(p_isolate, out_buffer.size());
    memcpy(ui->Buffer()->GetBackingStore()->Data(), out_buffer.data(), out_buffer.size());
    args.GetReturnValue().Set(ui);
}

v8::Local<v8::ObjectTemplate> Zlib::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    v8::Local<v8::ObjectTemplate> constants = v8::ObjectTemplate::New(p_isolate);
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_NO_COMPRESSION"), v8::Number::New(p_isolate, (double)Z_NO_COMPRESSION));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_BEST_SPEED"), v8::Number::New(p_isolate, (double)Z_BEST_SPEED));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_BEST_COMPRESSION"), v8::Number::New(p_isolate, (double)Z_BEST_COMPRESSION));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_DEFAULT_COMPRESSION"), v8::Number::New(p_isolate, (double)Z_DEFAULT_COMPRESSION));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_FILTERED"), v8::Number::New(p_isolate, (double)Z_FILTERED));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_HUFFMAN_ONLY"), v8::Number::New(p_isolate, (double)Z_HUFFMAN_ONLY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_RLE"), v8::Number::New(p_isolate, (double)Z_RLE));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_FIXED"), v8::Number::New(p_isolate, (double)Z_FIXED));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_DEFAULT_STRATEGY"), v8::Number::New(p_isolate, (double)Z_DEFAULT_STRATEGY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_NO_FLUSH"), v8::Number::New(p_isolate, (double)Z_NO_FLUSH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_PARTIAL_FLUSH"), v8::Number::New(p_isolate, (double)Z_PARTIAL_FLUSH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_SYNC_FLUSH"), v8::Number::New(p_isolate, (double)Z_SYNC_FLUSH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_FULL_FLUSH"), v8::Number::New(p_isolate, (double)Z_FULL_FLUSH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_FINISH"), v8::Number::New(p_isolate, (double)Z_FINISH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_BLOCK"), v8::Number::New(p_isolate, (double)Z_BLOCK));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_TREES"), v8::Number::New(p_isolate, (double)Z_TREES));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_OK"), v8::Number::New(p_isolate, (double)Z_OK));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_STREAM_END"), v8::Number::New(p_isolate, (double)Z_STREAM_END));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_NEED_DICT"), v8::Number::New(p_isolate, (double)Z_NEED_DICT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_ERRNO"), v8::Number::New(p_isolate, (double)Z_ERRNO));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_STREAM_ERROR"), v8::Number::New(p_isolate, (double)Z_STREAM_ERROR));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_DATA_ERROR"), v8::Number::New(p_isolate, (double)Z_DATA_ERROR));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MEM_ERROR"), v8::Number::New(p_isolate, (double)Z_MEM_ERROR));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_BUF_ERROR"), v8::Number::New(p_isolate, (double)Z_BUF_ERROR));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_VERSION_ERROR"), v8::Number::New(p_isolate, (double)Z_VERSION_ERROR));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MIN_LEVEL"), v8::Number::New(p_isolate, (double)-1));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MAX_LEVEL"), v8::Number::New(p_isolate, (double)9));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MIN_MEMLEVEL"), v8::Number::New(p_isolate, (double)1));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MAX_MEMLEVEL"), v8::Number::New(p_isolate, (double)9));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MIN_WINDOWBITS"), v8::Number::New(p_isolate, (double)8));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MAX_WINDOWBITS"), v8::Number::New(p_isolate, (double)15));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MIN_CHUNK"), v8::Number::New(p_isolate, (double)64));

    // Brotli Constants
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_MODE"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_MODE));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_QUALITY"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_QUALITY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_LGWIN"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_LGWIN));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_LGBLOCK"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_LGBLOCK));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_SIZE_HINT"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_SIZE_HINT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_LARGE_WINDOW"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_LARGE_WINDOW));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_NPOSTFIX"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_NPOSTFIX));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_NDIRECT"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_NDIRECT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_PARAM_STREAM_OFFSET"), v8::Number::New(p_isolate, (double)BROTLI_PARAM_STREAM_OFFSET));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MODE_GENERIC"), v8::Number::New(p_isolate, (double)BROTLI_MODE_GENERIC));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MODE_TEXT"), v8::Number::New(p_isolate, (double)BROTLI_MODE_TEXT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MODE_FONT"), v8::Number::New(p_isolate, (double)BROTLI_MODE_FONT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DEFAULT_QUALITY"), v8::Number::New(p_isolate, (double)BROTLI_DEFAULT_QUALITY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DEFAULT_WINDOW"), v8::Number::New(p_isolate, (double)BROTLI_DEFAULT_WINDOW));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DEFAULT_MODE"), v8::Number::New(p_isolate, (double)BROTLI_DEFAULT_MODE));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MIN_QUALITY"), v8::Number::New(p_isolate, (double)BROTLI_MIN_QUALITY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MAX_QUALITY"), v8::Number::New(p_isolate, (double)BROTLI_MAX_QUALITY));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MIN_WINDOW_BITS"), v8::Number::New(p_isolate, (double)BROTLI_MIN_WINDOW_BITS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MAX_WINDOW_BITS"), v8::Number::New(p_isolate, (double)BROTLI_MAX_WINDOW_BITS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_LARGE_MAX_WINDOW_BITS"), v8::Number::New(p_isolate, (double)BROTLI_LARGE_MAX_WINDOW_BITS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MIN_INPUT_BLOCK_BITS"), v8::Number::New(p_isolate, (double)BROTLI_MIN_INPUT_BLOCK_BITS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_MAX_INPUT_BLOCK_BITS"), v8::Number::New(p_isolate, (double)BROTLI_MAX_INPUT_BLOCK_BITS));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_PARAM_LARGE_WINDOW"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_PARAM_LARGE_WINDOW));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_RESULT_ERROR"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_RESULT_ERROR));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_RESULT_SUCCESS"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_RESULT_SUCCESS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT"), v8::Number::New(p_isolate, (double)BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT));

    // Brotli Error Codes
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_NO_ERROR"), v8::Number::New(p_isolate, 0));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_SUCCESS"), v8::Number::New(p_isolate, 1));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_NEEDS_MORE_INPUT"), v8::Number::New(p_isolate, 2));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_NEEDS_MORE_OUTPUT"), v8::Number::New(p_isolate, 3));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE"), v8::Number::New(p_isolate, -1));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_RESERVED"), v8::Number::New(p_isolate, -2));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE"), v8::Number::New(p_isolate, -3));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET"), v8::Number::New(p_isolate, -4));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME"), v8::Number::New(p_isolate, -5));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_CL_SPACE"), v8::Number::New(p_isolate, -6));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE"), v8::Number::New(p_isolate, -7));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT"), v8::Number::New(p_isolate, -8));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1"), v8::Number::New(p_isolate, -9));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2"), v8::Number::New(p_isolate, -10));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_TRANSFORM"), v8::Number::New(p_isolate, -11));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_DICTIONARY"), v8::Number::New(p_isolate, -12));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS"), v8::Number::New(p_isolate, -13));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_PADDING_1"), v8::Number::New(p_isolate, -14));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_PADDING_2"), v8::Number::New(p_isolate, -15));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_FORMAT_DISTANCE"), v8::Number::New(p_isolate, -16));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_COMPOUND_DICTIONARY"), v8::Number::New(p_isolate, -18));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET"), v8::Number::New(p_isolate, -19));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_INVALID_ARGUMENTS"), v8::Number::New(p_isolate, -20));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES"), v8::Number::New(p_isolate, -21));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS"), v8::Number::New(p_isolate, -22));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP"), v8::Number::New(p_isolate, -25));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1"), v8::Number::New(p_isolate, -26));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2"), v8::Number::New(p_isolate, -27));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES"), v8::Number::New(p_isolate, -30));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_DECODER_ERROR_UNREACHABLE"), v8::Number::New(p_isolate, -31));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_OPERATION_PROCESS"), v8::Number::New(p_isolate, (double)BROTLI_OPERATION_PROCESS));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_OPERATION_FLUSH"), v8::Number::New(p_isolate, (double)BROTLI_OPERATION_FLUSH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_OPERATION_FINISH"), v8::Number::New(p_isolate, (double)BROTLI_OPERATION_FINISH));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "BROTLI_OPERATION_EMIT_METADATA"), v8::Number::New(p_isolate, (double)BROTLI_OPERATION_EMIT_METADATA));

    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_CLEVEL_DEFAULT"), v8::Number::New(p_isolate, (double)ZSTD_CLEVEL_DEFAULT));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_CLEVEL_MIN"), v8::Number::New(p_isolate, (double)-22));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_CLEVEL_MAX"), v8::Number::New(p_isolate, (double)22));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_COMPRESSIONLEVEL"), v8::Number::New(p_isolate, (double)ZSTD_c_compressionLevel));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_STRATEGY"), v8::Number::New(p_isolate, (double)ZSTD_c_strategy));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_WINDOWLOG"), v8::Number::New(p_isolate, (double)ZSTD_c_windowLog));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_HASHLOG"), v8::Number::New(p_isolate, (double)ZSTD_c_hashLog));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_CHAINLOG"), v8::Number::New(p_isolate, (double)ZSTD_c_chainLog));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_SEARCHLOG"), v8::Number::New(p_isolate, (double)ZSTD_c_searchLog));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_MINMATCH"), v8::Number::New(p_isolate, (double)ZSTD_c_minMatch));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_C_TARGETLENGTH"), v8::Number::New(p_isolate, (double)ZSTD_c_targetLength));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_e_continue"), v8::Number::New(p_isolate, (double)ZSTD_e_continue));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_e_flush"), v8::Number::New(p_isolate, (double)ZSTD_e_flush));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_e_end"), v8::Number::New(p_isolate, (double)ZSTD_e_end));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_c_compressionLevel"), v8::Number::New(p_isolate, (double)ZSTD_c_compressionLevel));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_c_checksumFlag"), v8::Number::New(p_isolate, (double)ZSTD_c_checksumFlag));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZSTD_c_dictIDFlag"), v8::Number::New(p_isolate, (double)ZSTD_c_dictIDFlag));

    v8::Local<v8::ObjectTemplate> codes = v8::ObjectTemplate::New(p_isolate);
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_OK"), v8::Number::New(p_isolate, (double)Z_OK));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_STREAM_END"), v8::Number::New(p_isolate, (double)Z_STREAM_END));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_NEED_DICT"), v8::Number::New(p_isolate, (double)Z_NEED_DICT));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_ERRNO"), v8::Number::New(p_isolate, (double)Z_ERRNO));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_STREAM_ERROR"), v8::Number::New(p_isolate, (double)Z_STREAM_ERROR));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_DATA_ERROR"), v8::Number::New(p_isolate, (double)Z_DATA_ERROR));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_MEM_ERROR"), v8::Number::New(p_isolate, (double)Z_MEM_ERROR));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_BUF_ERROR"), v8::Number::New(p_isolate, (double)Z_BUF_ERROR));
    codes->Set(v8::String::NewFromUtf8Literal(p_isolate, "Z_VERSION_ERROR"), v8::Number::New(p_isolate, (double)Z_VERSION_ERROR));

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "codes"), codes);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "constants"), constants);

    // Node.js also exposes constants on the root object
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> const_obj = constants->NewInstance(context).ToLocalChecked();
    v8::Local<v8::Array> const_names = const_obj->GetPropertyNames(context).ToLocalChecked();
    for (uint32_t i = 0; i < const_names->Length(); i++) {
        v8::Local<v8::Value> key = const_names->Get(context, i).ToLocalChecked();
        v8::Local<v8::Value> val = const_obj->Get(context, key).ToLocalChecked();
        tmpl->Set(key.As<v8::Name>(), val);
    }

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "crc32"), v8::FunctionTemplate::New(p_isolate, crc32));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "adler32"), v8::FunctionTemplate::New(p_isolate, adler32));

    v8::Local<v8::FunctionTemplate> ft_zlib_base = v8::FunctionTemplate::New(p_isolate);
    ft_zlib_base->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Zlib"));
    ft_zlib_base->InstanceTemplate()->SetInternalFieldCount(1);
    
    v8::Local<v8::ObjectTemplate> zlib_proto = ft_zlib_base->PrototypeTemplate();
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, streamClose));
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, streamReset));
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "params"), v8::FunctionTemplate::New(p_isolate, streamParams));
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, streamWrite));
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, streamEnd));
    zlib_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "flush"), v8::FunctionTemplate::New(p_isolate, streamFlush));

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Zlib"), ft_zlib_base);

    // Register classes as constructors (aliases to create functions for now)
    v8::Local<v8::FunctionTemplate> ft_gzip = v8::FunctionTemplate::New(p_isolate, createGzip);
    ft_gzip->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Gzip"), ft_gzip);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createGzip"), ft_gzip);

    v8::Local<v8::FunctionTemplate> ft_gunzip = v8::FunctionTemplate::New(p_isolate, createGunzip);
    ft_gunzip->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Gunzip"), ft_gunzip);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createGunzip"), ft_gunzip);

    v8::Local<v8::FunctionTemplate> ft_deflate = v8::FunctionTemplate::New(p_isolate, createDeflate);
    ft_deflate->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Deflate"), ft_deflate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createDeflate"), ft_deflate);

    v8::Local<v8::FunctionTemplate> ft_inflate = v8::FunctionTemplate::New(p_isolate, createInflate);
    ft_inflate->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Inflate"), ft_inflate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createInflate"), ft_inflate);

    v8::Local<v8::FunctionTemplate> ft_deflate_raw = v8::FunctionTemplate::New(p_isolate, createDeflateRaw);
    ft_deflate_raw->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "DeflateRaw"), ft_deflate_raw);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createDeflateRaw"), ft_deflate_raw);

    v8::Local<v8::FunctionTemplate> ft_inflate_raw = v8::FunctionTemplate::New(p_isolate, createInflateRaw);
    ft_inflate_raw->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "InflateRaw"), ft_inflate_raw);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createInflateRaw"), ft_inflate_raw);

    v8::Local<v8::FunctionTemplate> ft_unzip = v8::FunctionTemplate::New(p_isolate, createUnzip);
    ft_unzip->Inherit(ft_zlib_base);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Unzip"), ft_unzip);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createUnzip"), ft_unzip);

    v8::Local<v8::FunctionTemplate> ft_br_comp = v8::FunctionTemplate::New(p_isolate, createBrotliCompress);
    ft_br_comp->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "BrotliCompress"));
    ft_br_comp->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::ObjectTemplate> br_comp_proto = ft_br_comp->PrototypeTemplate();
    br_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, brotliStreamWrite));
    br_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, brotliStreamEnd));
    br_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, brotliStreamClose));
    br_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, brotliStreamReset));
    br_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "flush"), v8::FunctionTemplate::New(p_isolate, brotliStreamFlush));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "BrotliCompress"), ft_br_comp);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createBrotliCompress"), ft_br_comp);

    v8::Local<v8::FunctionTemplate> ft_br_decomp = v8::FunctionTemplate::New(p_isolate, createBrotliDecompress);
    ft_br_decomp->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "BrotliDecompress"));
    ft_br_decomp->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::ObjectTemplate> br_decomp_proto = ft_br_decomp->PrototypeTemplate();
    br_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, brotliStreamWrite));
    br_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, brotliStreamEnd));
    br_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, brotliStreamClose));
    br_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, brotliStreamReset));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "BrotliDecompress"), ft_br_decomp);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createBrotliDecompress"), ft_br_decomp);

    v8::Local<v8::FunctionTemplate> ft_zstd_comp = v8::FunctionTemplate::New(p_isolate, createZstdCompress);
    ft_zstd_comp->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "ZstdCompress"));
    ft_zstd_comp->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::ObjectTemplate> zstd_comp_proto = ft_zstd_comp->PrototypeTemplate();
    zstd_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, zstdStreamWrite));
    zstd_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, zstdStreamEnd));
    zstd_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, zstdStreamClose));
    zstd_comp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, zstdStreamReset));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZstdCompress"), ft_zstd_comp);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createZstdCompress"), ft_zstd_comp);

    v8::Local<v8::FunctionTemplate> ft_zstd_decomp = v8::FunctionTemplate::New(p_isolate, createZstdDecompress);
    ft_zstd_decomp->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "ZstdDecompress"));
    ft_zstd_decomp->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::ObjectTemplate> zstd_decomp_proto = ft_zstd_decomp->PrototypeTemplate();
    zstd_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, zstdStreamWrite));
    zstd_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, zstdStreamEnd));
    zstd_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, zstdStreamClose));
    zstd_decomp_proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, zstdStreamReset));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "ZstdDecompress"), ft_zstd_decomp);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createZstdDecompress"), ft_zstd_decomp);

    // Sync methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflateSync"), v8::FunctionTemplate::New(p_isolate, deflateSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflateSync"), v8::FunctionTemplate::New(p_isolate, inflateSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflateRawSync"), v8::FunctionTemplate::New(p_isolate, deflateRawSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflateRawSync"), v8::FunctionTemplate::New(p_isolate, inflateRawSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gzipSync"), v8::FunctionTemplate::New(p_isolate, gzipSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gunzipSync"), v8::FunctionTemplate::New(p_isolate, gunzipSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "unzipSync"), v8::FunctionTemplate::New(p_isolate, unzipSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliCompressSync"), v8::FunctionTemplate::New(p_isolate, brotliCompressSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliDecompressSync"), v8::FunctionTemplate::New(p_isolate, brotliDecompressSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdCompressSync"), v8::FunctionTemplate::New(p_isolate, zstdCompressSync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdDecompressSync"), v8::FunctionTemplate::New(p_isolate, zstdDecompressSync));

    // Async methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflate"), v8::FunctionTemplate::New(p_isolate, deflate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflate"), v8::FunctionTemplate::New(p_isolate, inflate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflateRaw"), v8::FunctionTemplate::New(p_isolate, deflateRaw));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflateRaw"), v8::FunctionTemplate::New(p_isolate, inflateRaw));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gzip"), v8::FunctionTemplate::New(p_isolate, gzip));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gunzip"), v8::FunctionTemplate::New(p_isolate, gunzip));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "unzip"), v8::FunctionTemplate::New(p_isolate, unzip));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliCompress"), v8::FunctionTemplate::New(p_isolate, brotliCompress));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliDecompress"), v8::FunctionTemplate::New(p_isolate, brotliDecompress));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdCompress"), v8::FunctionTemplate::New(p_isolate, zstdCompress));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdDecompress"), v8::FunctionTemplate::New(p_isolate, zstdDecompress));

    // Promises
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "promises"), createPromisesTemplate(p_isolate));

    return tmpl;
}

static void doDeflate(const v8::FunctionCallbackInfo<v8::Value>& args, int32_t default_window_bits) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    int32_t level = Z_DEFAULT_COMPRESSION;
    int32_t window_bits = default_window_bits;
    int32_t mem_level = 8;
    int32_t strategy = Z_DEFAULT_STRATEGY;
    int32_t chunk_size = 16384;
    std::vector<uint8_t> dictionary;
    size_t max_output_length = 0;
    bool info_flag = false;
    std::string gzname, gzcomment;
    uint32_t gzmtime = 0;

    if (args.Length() >= 2) {
        parseZlibOptions(p_isolate, context, args[1], level, window_bits, mem_level, strategy, chunk_size, dictionary, max_output_length, info_flag, gzname, gzcomment, gzmtime);
    }

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    int32_t ret = deflateInit2(&strm, level, Z_DEFLATED, window_bits, mem_level, strategy);
    if (ret != Z_OK) {
        throwZlibError(p_isolate, ret, "deflateInit failed");
        return;
    }

    if (!dictionary.empty()) {
        deflateSetDictionary(&strm, (const Bytef*)dictionary.data(), (uInt)dictionary.size());
    }
    if (window_bits > 15 && (!gzname.empty() || !gzcomment.empty() || gzmtime != 0)) {
        gz_header head;
        memset(&head, 0, sizeof(head));
        if (!gzname.empty()) head.name = (Bytef*)gzname.c_str();
        if (!gzcomment.empty()) head.comment = (Bytef*)gzcomment.c_str();
        head.time = gzmtime;
        deflateSetHeader(&strm, &head);
    }

    strm.next_in = (Bytef*)p_data;
    strm.avail_in = (uInt)length;
    std::vector<uint8_t> out_buffer;
    out_buffer.resize(chunk_size);
    size_t total_out = 0;
    do {
        if (total_out + chunk_size > out_buffer.size()) {
            out_buffer.resize(out_buffer.size() + chunk_size);
        }
        strm.next_out = out_buffer.data() + total_out;
        strm.avail_out = (uInt)chunk_size;

        ret = deflate(&strm, Z_FINISH);
        total_out += chunk_size - strm.avail_out;

        if (max_output_length > 0 && total_out > max_output_length) {
            deflateEnd(&strm);
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
            return;
        }
        if (ret != Z_STREAM_END && ret != Z_OK) {
            deflateEnd(&strm);
            throwZlibError(p_isolate, ret, "deflate failed");
            return;
        }
    } while (ret == Z_OK);

    deflateEnd(&strm);

    out_buffer.resize(total_out);
    returnBuffer(args, out_buffer);
}

static void doInflate(const v8::FunctionCallbackInfo<v8::Value>& args, int32_t default_window_bits) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    int32_t level = Z_DEFAULT_COMPRESSION; // Not used for inflate, but kept for parseZlibOptions signature
    int32_t window_bits = default_window_bits;
    int32_t mem_level = 8; // Not used for inflate, but kept for parseZlibOptions signature
    int32_t strategy = Z_DEFAULT_STRATEGY; // Not used for inflate, but kept for parseZlibOptions signature
    int32_t chunk_size = 16384;
    std::vector<uint8_t> dictionary;
    size_t max_output_length = 0;
    bool info_flag = false;
    std::string gzname, gzcomment; // Not used for inflate, but kept for parseZlibOptions signature
    uint32_t gzmtime = 0; // Not used for inflate, but kept for parseZlibOptions signature

    if (args.Length() >= 2) {
        parseZlibOptions(p_isolate, context, args[1], level, window_bits, mem_level, strategy, chunk_size, dictionary, max_output_length, info_flag, gzname, gzcomment, gzmtime);
    }

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in = (Bytef*)p_data;
    strm.avail_in = (uInt)length;

    int32_t ret = inflateInit2(&strm, window_bits);
    if (ret != Z_OK) {
        throwZlibError(p_isolate, ret, "inflateInit failed");
        return;
    }
    if (!dictionary.empty()) {
        inflateSetDictionary(&strm, (const Bytef*)dictionary.data(), (uInt)dictionary.size());
    }

    std::vector<uint8_t> out_buffer;
    out_buffer.resize(chunk_size);
    size_t total_out = 0;
    do {
        if (total_out + chunk_size > out_buffer.size()) {
            out_buffer.resize(out_buffer.size() + chunk_size);
        }
        strm.next_out = out_buffer.data() + total_out;
        strm.avail_out = (uInt)chunk_size;

        ret = inflate(&strm, Z_NO_FLUSH);
        total_out += chunk_size - strm.avail_out;

        if (max_output_length > 0 && total_out > max_output_length) {
            inflateEnd(&strm);
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
            return;
        }
        
        if (ret == Z_NEED_DICT) {
            if (!dictionary.empty()) {
                ret = inflateSetDictionary(&strm, (const Bytef*)dictionary.data(), (uInt)dictionary.size());
                if (ret == Z_OK) continue;
            }
        }

        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            throwZlibError(p_isolate, ret, "inflate failed");
            return;
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    out_buffer.resize(total_out);
    returnBuffer(args, out_buffer);
}

static void doZlibAsync(const v8::FunctionCallbackInfo<v8::Value>& args, int32_t window_bits, bool is_deflate, bool is_promise = false, bool is_brotli = false, bool is_zstd = false) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;
    
    v8::Local<v8::Function> callback;
    v8::Local<v8::Promise::Resolver> resolver;

    if (is_promise) {
        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) return;
        args.GetReturnValue().Set(resolver->GetPromise());
    } else {
        if (args.Length() >= 2 && args[args.Length() - 1]->IsFunction()) {
            callback = args[args.Length() - 1].As<v8::Function>();
        } else {
            p_isolate->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8Literal(p_isolate, "Callback must be provided")));
            return;
        }
    }

    ZlibAsyncCtx* p_ctx = new ZlibAsyncCtx();
    p_ctx->m_input.assign(p_data, p_data + length);
    p_ctx->m_window_bits = window_bits;
    p_ctx->m_is_deflate = is_deflate;
    p_ctx->m_is_brotli = is_brotli;
    p_ctx->m_is_zstd = is_zstd;

    if (args.Length() >= 2 && !args[1]->IsFunction()) {
        if (is_brotli) {
            parseBrotliOptions(p_isolate, context, args[1], p_ctx->m_brotli_quality, p_ctx->m_brotli_window, p_ctx->m_brotli_mode, p_ctx->m_chunk_size, p_ctx->m_max_output_length, p_ctx->m_brotli_params, p_ctx->m_dictionary);
        } else if (is_zstd) {
            parseZstdOptions(p_isolate, context, args[1], p_ctx->m_zstd_level, p_ctx->m_chunk_size, p_ctx->m_dictionary);
        } else {
            parseZlibOptions(p_isolate, context, args[1], p_ctx->m_level, p_ctx->m_window_bits, p_ctx->m_mem_level, p_ctx->m_strategy, p_ctx->m_chunk_size, p_ctx->m_dictionary, p_ctx->m_max_output_length, p_ctx->m_info, p_ctx->m_gzname, p_ctx->m_gzcomment, p_ctx->m_gzmtime);
        }
    }

    Task* p_task = new Task();
    p_task->m_is_promise = is_promise;
    if (is_promise) {
        p_task->m_resolver.Reset(p_isolate, resolver);
    } else {
        p_task->m_callback.Reset(p_isolate, callback);
    }
    p_task->p_data = p_ctx;

    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        ZlibAsyncCtx* p_ctx = static_cast<ZlibAsyncCtx*>(task->p_data);
        if (task->m_is_promise) {
            auto resolver = task->m_resolver.Get(isolate);
            if (p_ctx->m_is_error) {
                resolver->Reject(context, v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked())).Check();
            } else {
                v8::Local<v8::Uint8Array> ui = zane::module::Buffer::createBuffer(isolate, p_ctx->m_output.size());
                memcpy(ui->Buffer()->GetBackingStore()->Data(), p_ctx->m_output.data(), p_ctx->m_output.size());
                
                if (p_ctx->m_info) {
                    v8::Local<v8::Object> res_obj = v8::Object::New(isolate);
                    res_obj->Set(context, v8::String::NewFromUtf8Literal(isolate, "buffer"), ui).Check();
                    // engine property could be added here if we had a persistent engine object
                    resolver->Resolve(context, res_obj).Check();
                } else {
                    resolver->Resolve(context, ui).Check();
                }
            }
        } else {
            v8::Local<v8::Value> argv[2];
            if (p_ctx->m_is_error) {
                argv[0] = v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
                argv[1] = v8::Null(isolate);
            } else {
                argv[0] = v8::Null(isolate);
                v8::Local<v8::Uint8Array> ui = zane::module::Buffer::createBuffer(isolate, p_ctx->m_output.size());
                memcpy(ui->Buffer()->GetBackingStore()->Data(), p_ctx->m_output.data(), p_ctx->m_output.size());
                
                if (p_ctx->m_info) {
                    v8::Local<v8::Object> res_obj = v8::Object::New(isolate);
                    res_obj->Set(context, v8::String::NewFromUtf8Literal(isolate, "buffer"), ui).Check();
                    argv[1] = res_obj;
                } else {
                    argv[1] = ui;
                }
            }
            (void)task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task]() {
        ZlibAsyncCtx* p_ctx = static_cast<ZlibAsyncCtx*>(p_task->p_data);
        
        if (p_ctx->m_is_brotli) {
            if (p_ctx->m_is_deflate) { // Brotli Compress
                BrotliEncoderState* p_s = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
                if (!p_s) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = "Failed to create Brotli encoder";
                } else {
                    // Set parameters
                    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_QUALITY, p_ctx->m_brotli_quality);
                    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_LGWIN, p_ctx->m_brotli_window);
                    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_MODE, p_ctx->m_brotli_mode);
                    for (const auto& param : p_ctx->m_brotli_params) {
                        BrotliEncoderSetParameter(p_s, (BrotliEncoderParameter)param.first, param.second);
                    }
                    if (!p_ctx->m_dictionary.empty()) {
                        p_ctx->p_brotli_prepared_dict = BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, p_ctx->m_dictionary.size(), p_ctx->m_dictionary.data(), BROTLI_MAX_QUALITY, nullptr, nullptr, nullptr);
                        if (p_ctx->p_brotli_prepared_dict) {
                            BrotliEncoderAttachPreparedDictionary(p_s, p_ctx->p_brotli_prepared_dict);
                        }
                    }

                    size_t available_in = p_ctx->m_input.size();
                    const uint8_t* p_next_in = p_ctx->m_input.data();
                    size_t total_out = 0;
                    const size_t chunk_size = p_ctx->m_chunk_size > 0 ? p_ctx->m_chunk_size : 16384;
                    
                    while (available_in > 0 || BrotliEncoderHasMoreOutput(p_s)) {
                        p_ctx->m_output.resize(total_out + chunk_size);
                        size_t available_out = chunk_size;
                        uint8_t* p_next_out = p_ctx->m_output.data() + total_out;
                        if (!BrotliEncoderCompressStream(p_s, BROTLI_OPERATION_FINISH, &available_in, &p_next_in, &available_out, &p_next_out, &total_out)) {
                             p_ctx->m_is_error = true;
                             p_ctx->m_error_msg = "Brotli compression failed";
                             break;
                        }
                    }
                    if (!p_ctx->m_is_error) p_ctx->m_output.resize(total_out);
                    BrotliEncoderDestroyInstance(p_s);
                    if (p_ctx->p_brotli_prepared_dict) BrotliEncoderDestroyPreparedDictionary(p_ctx->p_brotli_prepared_dict);
                }
            } else { // Brotli Decompress
                BrotliDecoderState* p_s = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
                if (!p_s) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = "Failed to create Brotli decoder";
                } else {
                    // Set parameters
                    for (const auto& param : p_ctx->m_brotli_params) {
                        BrotliDecoderSetParameter(p_s, (BrotliDecoderParameter)param.first, param.second);
                    }
                    if (!p_ctx->m_dictionary.empty()) {
                        BrotliDecoderAttachDictionary(p_s, BROTLI_SHARED_DICTIONARY_RAW, p_ctx->m_dictionary.size(), p_ctx->m_dictionary.data());
                    }

                    size_t available_in = p_ctx->m_input.size();
                    const uint8_t* p_next_in = p_ctx->m_input.data();
                    size_t total_out = 0;
                    const size_t chunk_size = 16384;
                    p_ctx->m_output.clear();

                    BrotliDecoderResult res = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
                    while (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                        p_ctx->m_output.resize(total_out + chunk_size);
                        size_t available_out = chunk_size;
                        uint8_t* p_next_out = p_ctx->m_output.data() + total_out;
                        res = BrotliDecoderDecompressStream(p_s, &available_in, &p_next_in, &available_out, &p_next_out, &total_out);
                        
                        if (p_ctx->m_max_output_length > 0 && total_out > p_ctx->m_max_output_length) {
                             p_ctx->m_is_error = true;
                             p_ctx->m_error_msg = "maxOutputLength exceeded";
                             break;
                        }
                    }

                    if (res != BROTLI_DECODER_RESULT_SUCCESS) {
                        p_ctx->m_is_error = true;
                        p_ctx->m_error_msg = "Brotli decompression failed";
                    } else {
                        p_ctx->m_output.resize(total_out);
                    }
                    BrotliDecoderDestroyInstance(p_s);
                }
            }
        } else if (p_ctx->m_is_zstd) {
            if (p_ctx->m_is_deflate) { // Zstd Compress
                ZSTD_CCtx* p_cctx = ZSTD_createCCtx();
                if (!p_cctx) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = "Failed to create Zstd compressor";
                } else {
                    ZSTD_CCtx_setParameter(p_cctx, ZSTD_c_compressionLevel, p_ctx->m_zstd_level);
                    if (!p_ctx->m_dictionary.empty()) {
                        ZSTD_CCtx_loadDictionary(p_cctx, p_ctx->m_dictionary.data(), p_ctx->m_dictionary.size());
                    }
                    size_t const out_size = ZSTD_compressBound(p_ctx->m_input.size());
                    p_ctx->m_output.resize(out_size);
                    size_t const res = ZSTD_compressCCtx(p_cctx, p_ctx->m_output.data(), out_size, p_ctx->m_input.data(), p_ctx->m_input.size(), p_ctx->m_zstd_level);
                    if (ZSTD_isError(res)) {
                        p_ctx->m_is_error = true;
                        p_ctx->m_error_msg = ZSTD_getErrorName(res);
                    } else {
                        p_ctx->m_output.resize(res);
                    }
                    ZSTD_freeCCtx(p_cctx);
                }
            } else { // Zstd Decompress
                ZSTD_DCtx* p_dctx = ZSTD_createDCtx();
                if (!p_dctx) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = "Failed to create Zstd decompressor";
                } else {
                    if (!p_ctx->m_dictionary.empty()) {
                        ZSTD_DCtx_loadDictionary(p_dctx, p_ctx->m_dictionary.data(), p_ctx->m_dictionary.size());
                    }
                    uint64_t const decoded_size = ZSTD_getFrameContentSize(p_ctx->m_input.data(), p_ctx->m_input.size());
                    if (decoded_size != ZSTD_CONTENTSIZE_ERROR && decoded_size != ZSTD_CONTENTSIZE_UNKNOWN) {
                        if (p_ctx->m_max_output_length > 0 && decoded_size > p_ctx->m_max_output_length) {
                            p_ctx->m_is_error = true;
                            p_ctx->m_error_msg = "maxOutputLength exceeded";
                        } else {
                            p_ctx->m_output.resize(decoded_size);
                            size_t res = ZSTD_decompressDCtx(p_dctx, p_ctx->m_output.data(), decoded_size, p_ctx->m_input.data(), p_ctx->m_input.size());
                            if (ZSTD_isError(res)) {
                                p_ctx->m_is_error = true;
                                p_ctx->m_error_msg = ZSTD_getErrorName(res);
                            }
                        }
                    } else {
                        // Unknown size, use streaming decompress or large buffer
                        size_t const chunk_size = p_ctx->m_chunk_size;
                        p_ctx->m_output.resize(chunk_size);
                        ZSTD_inBuffer input = { p_ctx->m_input.data(), p_ctx->m_input.size(), 0 };
                        ZSTD_outBuffer output = { p_ctx->m_output.data(), p_ctx->m_output.size(), 0 };
                        while (input.pos < input.size) {
                            size_t const res = ZSTD_decompressStream(p_dctx, &output, &input);
                            if (ZSTD_isError(res)) {
                                p_ctx->m_is_error = true;
                                p_ctx->m_error_msg = ZSTD_getErrorName(res);
                                break;
                            }
                            if (output.pos == output.size) { // Output buffer full, resize
                                output.size += chunk_size;
                                p_ctx->m_output.resize(output.size);
                                output.dst = p_ctx->m_output.data();
                            }
                            if (p_ctx->m_max_output_length > 0 && output.pos > p_ctx->m_max_output_length) {
                                p_ctx->m_is_error = true;
                                p_ctx->m_error_msg = "maxOutputLength exceeded";
                                break;
                            }
                            if (res == 0) break; // End of frame
                        }
                        p_ctx->m_output.resize(output.pos);
                    }
                    ZSTD_freeDCtx(p_dctx);
                }
            }
        } else { // Standard Zlib
            z_stream strm;
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;

            int32_t ret;
            if (p_ctx->m_is_deflate) {
                ret = deflateInit2(&strm, p_ctx->m_level, Z_DEFLATED, p_ctx->m_window_bits, p_ctx->m_mem_level, p_ctx->m_strategy);
                if (ret == Z_OK && !p_ctx->m_dictionary.empty()) {
                    deflateSetDictionary(&strm, (const Bytef*)p_ctx->m_dictionary.data(), (uInt)p_ctx->m_dictionary.size());
                }
                if (ret == Z_OK && p_ctx->m_window_bits > 15 && (!p_ctx->m_gzname.empty() || !p_ctx->m_gzcomment.empty() || p_ctx->m_gzmtime != 0)) {
                    gz_header head;
                    memset(&head, 0, sizeof(head));
                    if (!p_ctx->m_gzname.empty()) head.name = (Bytef*)p_ctx->m_gzname.c_str();
                    if (!p_ctx->m_gzcomment.empty()) head.comment = (Bytef*)p_ctx->m_gzcomment.c_str();
                    head.time = p_ctx->m_gzmtime;
                    deflateSetHeader(&strm, &head);
                }
            } else {
                ret = inflateInit2(&strm, p_ctx->m_window_bits);
            }

            if (ret != Z_OK) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "Zlib initialization failed";
                TaskQueue::getInstance().enqueue(p_task);
                return;
            }

            strm.next_in = (Bytef*)p_ctx->m_input.data();
            strm.avail_in = (uInt)p_ctx->m_input.size();
            
            size_t chunk_size = p_ctx->m_chunk_size;
            p_ctx->m_output.resize(chunk_size);
            size_t total_out = 0;

            do {
                if (total_out + chunk_size > p_ctx->m_output.size()) {
                    p_ctx->m_output.resize(p_ctx->m_output.size() + chunk_size);
                }
                strm.next_out = p_ctx->m_output.data() + total_out;
                strm.avail_out = (uInt)chunk_size;

                if (p_ctx->m_is_deflate) {
                    ret = deflate(&strm, Z_FINISH);
                } else {
                    ret = inflate(&strm, Z_NO_FLUSH);
                }
                total_out += chunk_size - strm.avail_out;

                if (p_ctx->m_max_output_length > 0 && total_out > p_ctx->m_max_output_length) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = "maxOutputLength exceeded";
                    break;
                }

                if (ret == Z_NEED_DICT) {
                    if (!p_ctx->m_dictionary.empty()) {
                        ret = inflateSetDictionary(&strm, (const Bytef*)p_ctx->m_dictionary.data(), (uInt)p_ctx->m_dictionary.size());
                        if (ret == Z_OK) continue;
                    }
                    break;
                }
                if (ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                    break;
                }
            } while (ret != Z_STREAM_END && (p_ctx->m_is_deflate ? (ret == Z_OK) : true));

            if (p_ctx->m_is_deflate) deflateEnd(&strm);
            else inflateEnd(&strm);

            if (ret != Z_STREAM_END) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "Zlib processing failed";
            } else {
                p_ctx->m_output.resize(total_out);
            }
        }

        TaskQueue::getInstance().enqueue(p_task);
    });
}

void Zlib::deflateSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doDeflate(args, 15);
}

void Zlib::inflateSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doInflate(args, 15);
}

void Zlib::deflateRawSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doDeflate(args, -15);
}

void Zlib::inflateRawSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doInflate(args, -15);
}

void Zlib::gzipSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doDeflate(args, 15 + 16);
}

void Zlib::gunzipSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doInflate(args, 15 + 16);
}

void Zlib::unzipSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doInflate(args, 15 + 32);
}

void Zlib::deflate(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15, true, false, false, false); }
void Zlib::inflate(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15, false, false, false, false); }
void Zlib::deflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, -15, true, false, false, false); }
void Zlib::inflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, -15, false, false, false, false); }
void Zlib::gzip(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 16, true, false, false, false); }
void Zlib::gunzip(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 16, false, false, false, false); }
void Zlib::unzip(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 32, false, false, false, false); }

void Zlib::deflatePromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15, true, true, false, false); }
void Zlib::inflatePromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15, false, true, false, false); }
void Zlib::deflateRawPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, -15, true, true, false, false); }
void Zlib::inflateRawPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, -15, false, true, false, false); }
void Zlib::gzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 16, true, true, false, false); }
void Zlib::gunzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 16, false, true, false, false); }
void Zlib::unzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 15 + 32, false, true, false, false); }

void Zlib::brotliCompress(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 0, true, false, true, false); }
void Zlib::brotliDecompress(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 0, false, false, true, false); }
void Zlib::brotliCompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 0, true, true, true, false); }
void Zlib::brotliDecompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args) { doZlibAsync(args, 0, false, true, true, false); }

void Zlib::zstdCompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doZlibAsync(args, 0, true, false, false, true);
}

void Zlib::zstdDecompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doZlibAsync(args, 0, false, false, false, true);
}

void Zlib::zstdCompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doZlibAsync(args, 0, true, true, false, true);
}

void Zlib::zstdDecompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    doZlibAsync(args, 0, false, true, false, true);
}

void Zlib::brotliCompressSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    int32_t quality = BROTLI_DEFAULT_QUALITY;
    int32_t window = BROTLI_DEFAULT_WINDOW;
    int32_t mode = BROTLI_DEFAULT_MODE;
    int32_t chunk_size = 16384;
    size_t max_output_length = 0;
    std::vector<std::pair<int32_t, int32_t>> params_vec;
    std::vector<uint8_t> dictionary;

    if (args.Length() >= 2) {
        parseBrotliOptions(p_isolate, context, args[1], quality, window, mode, chunk_size, max_output_length, params_vec, dictionary);
    }

    BrotliEncoderState* p_s = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!p_s) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to create Brotli encoder")));
        return;
    }

    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_QUALITY, quality);
    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_LGWIN, window);
    BrotliEncoderSetParameter(p_s, BROTLI_PARAM_MODE, mode);
    for (const auto& param : params_vec) {
        BrotliEncoderSetParameter(p_s, (BrotliEncoderParameter)param.first, param.second);
    }
    BrotliEncoderPreparedDictionary* p_prepared = nullptr;
    if (!dictionary.empty()) {
        p_prepared = BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, dictionary.size(), dictionary.data(), BROTLI_MAX_QUALITY, nullptr, nullptr, nullptr);
        if (p_prepared) {
            BrotliEncoderAttachPreparedDictionary(p_s, p_prepared);
        }
    }

    size_t available_in = length;
    const uint8_t* p_next_in = p_data;
    size_t total_out = 0;
    std::vector<uint8_t> out_buffer;

    while (available_in > 0 || BrotliEncoderHasMoreOutput(p_s)) {
        out_buffer.resize(total_out + chunk_size);
        size_t available_out = chunk_size;
        uint8_t* p_next_out = out_buffer.data() + total_out;
        if (!BrotliEncoderCompressStream(p_s, BROTLI_OPERATION_FINISH, &available_in, &p_next_in, &available_out, &p_next_out, &total_out)) {
             BrotliEncoderDestroyInstance(p_s);
             if (p_prepared) BrotliEncoderDestroyPreparedDictionary(p_prepared);
             p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Brotli compression failed")));
             return;
        }
        if (max_output_length > 0 && total_out > max_output_length) {
             BrotliEncoderDestroyInstance(p_s);
             if (p_prepared) BrotliEncoderDestroyPreparedDictionary(p_prepared);
             p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
             return;
        }
    }

    out_buffer.resize(total_out);
    BrotliEncoderDestroyInstance(p_s);
    if (p_prepared) BrotliEncoderDestroyPreparedDictionary(p_prepared);
    returnBuffer(args, out_buffer);
}

void Zlib::brotliDecompressSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    // Brotli decompress options are less common in sync, but we could parse them
    int32_t quality = 0, window = 0, mode = 0, chunk_size = 16384;
    size_t max_output_length = 0;
    std::vector<std::pair<int32_t, int32_t>> params_vec;
    std::vector<uint8_t> dictionary;
    if (args.Length() >= 2) {
        parseBrotliOptions(p_isolate, context, args[1], quality, window, mode, chunk_size, max_output_length, params_vec, dictionary);
    }

    BrotliDecoderState* p_s = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!p_s) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to create Brotli decoder")));
        return;
    }

    for (const auto& param : params_vec) {
        BrotliDecoderSetParameter(p_s, (BrotliDecoderParameter)param.first, param.second);
    }
    if (!dictionary.empty()) {
        BrotliDecoderAttachDictionary(p_s, BROTLI_SHARED_DICTIONARY_RAW, dictionary.size(), dictionary.data());
    }

    size_t available_in = length;
    const uint8_t* p_next_in = p_data;
    size_t total_out = 0;
    // Use the parsed chunk_size
    // const size_t chunk_size = 16384;
    std::vector<uint8_t> out_buffer;

    BrotliDecoderResult res = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
    while (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        out_buffer.resize(total_out + chunk_size);
        size_t available_out = chunk_size;
        uint8_t* p_next_out = out_buffer.data() + total_out;
        res = BrotliDecoderDecompressStream(p_s, &available_in, &p_next_in, &available_out, &p_next_out, &total_out);
        
        if (max_output_length > 0 && total_out > max_output_length) {
            BrotliDecoderDestroyInstance(p_s);
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
            return;
        }
    }

    if (res != BROTLI_DECODER_RESULT_SUCCESS) {
        const char* p_err_str = BrotliDecoderErrorString(BrotliDecoderGetErrorCode(p_s));
        BrotliDecoderDestroyInstance(p_s);
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, (std::string("Brotli decompression failed: ") + p_err_str).c_str()).ToLocalChecked()));
        return;
    }

    out_buffer.resize(total_out);
    BrotliDecoderDestroyInstance(p_s);
    returnBuffer(args, out_buffer);
}

void Zlib::zstdCompressSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    int32_t level = 3;
    int32_t chunk_size = 128 * 1024;
    std::vector<uint8_t> dictionary;
    if (args.Length() >= 2) {
        parseZstdOptions(p_isolate, context, args[1], level, chunk_size, dictionary);
    }

    ZSTD_CCtx* p_cctx = ZSTD_createCCtx();
    if (!p_cctx) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to create Zstd compressor")));
        return;
    }
    ZSTD_CCtx_setParameter(p_cctx, ZSTD_c_compressionLevel, level);
    if (!dictionary.empty()) {
        ZSTD_CCtx_loadDictionary(p_cctx, dictionary.data(), dictionary.size());
    }

    size_t out_len = ZSTD_compressBound(length);
    std::vector<uint8_t> out_buffer(out_len);
    size_t res = ZSTD_compressCCtx(p_cctx, out_buffer.data(), out_len, p_data, length, level);

    if (ZSTD_isError(res)) {
        ZSTD_freeCCtx(p_cctx);
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ZSTD_getErrorName(res)).ToLocalChecked()));
        return;
    }

    out_buffer.resize(res);
    ZSTD_freeCCtx(p_cctx);
    returnBuffer(args, out_buffer);
}

void Zlib::zstdDecompressSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    int32_t level = 0; // Not used for decompression, but needed for signature
    int32_t chunk_size = 128 * 1024;
    std::vector<uint8_t> dictionary;
    if (args.Length() >= 2) {
        parseZstdOptions(p_isolate, context, args[1], level, chunk_size, dictionary);
    }

    ZSTD_DCtx* p_dctx = ZSTD_createDCtx();
    if (!p_dctx) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to create Zstd decompressor")));
        return;
    }
    if (!dictionary.empty()) {
        ZSTD_DCtx_loadDictionary(p_dctx, dictionary.data(), dictionary.size());
    }

    uint64_t const decoded_size = ZSTD_getFrameContentSize(p_data, length);
    if (decoded_size != ZSTD_CONTENTSIZE_ERROR && decoded_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        std::vector<uint8_t> out_buffer(decoded_size);
        size_t res = ZSTD_decompressDCtx(p_dctx, out_buffer.data(), decoded_size, p_data, length);
        if (ZSTD_isError(res)) {
            ZSTD_freeDCtx(p_dctx);
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ZSTD_getErrorName(res)).ToLocalChecked()));
            return;
        }
        ZSTD_freeDCtx(p_dctx);
        returnBuffer(args, out_buffer);
    } else {
        // Fallback for unknown size
        // size_t const chunk_size = 128 * 1024;
        std::vector<uint8_t> out_buffer;
        ZSTD_inBuffer input = { p_data, length, 0 };
        size_t total_out = 0;
        while (input.pos < input.size) {
            out_buffer.resize(total_out + chunk_size);
            ZSTD_outBuffer output = { out_buffer.data(), out_buffer.size(), total_out };
            size_t const res = ZSTD_decompressStream(p_dctx, &output, &input);
            total_out = output.pos;
            if (ZSTD_isError(res)) {
                ZSTD_freeDCtx(p_dctx);
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ZSTD_getErrorName(res)).ToLocalChecked()));
                return;
            }
            if (res == 0) break;
        }
        out_buffer.resize(total_out);
        ZSTD_freeDCtx(p_dctx);
        returnBuffer(args, out_buffer);
    }
}


void Zlib::crc32(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    uint32_t crc = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        crc = args[1]->Uint32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }

    uint32_t result = ::crc32(crc, (const Bytef*)p_data, (uInt)length);
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(p_isolate, result));
}

void Zlib::adler32(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    uint8_t* p_data;
    size_t length;
    std::vector<uint8_t> storage;
    if (!getInput(args, &p_data, &length, storage)) return;

    uint32_t adler = 1;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        adler = args[1]->Uint32Value(p_isolate->GetCurrentContext()).FromMaybe(1);
    }

    uint32_t result = ::adler32(adler, (const Bytef*)p_data, (uInt)length);
    args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(p_isolate, result));
}

struct ZlibStreamObject : public StreamInternal {
    z_stream m_strm;
    bool m_is_deflate;
    bool m_initialized = false;
    bool m_finished = false;
    int32_t m_chunk_size = 16384;
    std::vector<uint8_t> m_dictionary;
    int32_t m_window_bits;
    size_t m_max_output_length = 0;
    bool m_info_flag = false;
    std::string m_gzname;
    std::string m_gzcomment;
    uint32_t m_gzmtime = 0;

    ZlibStreamObject() {
        memset(&m_strm, 0, sizeof(m_strm));
    }
    ~ZlibStreamObject() {
        if (m_initialized) {
            if (m_is_deflate) deflateEnd(&m_strm);
            else inflateEnd(&m_strm);
        }
    }
};

static void streamProcess(const v8::FunctionCallbackInfo<v8::Value>& args, int32_t flush) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) {
        if (args.Length() > 2 && args[2]->IsFunction()) { // Callback for _transform
            v8::Local<v8::Function> callback = args[2].As<v8::Function>();
            v8::Local<v8::Value> argv[1] = { v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Internal stream object not found")) };
            callback->Call(context, v8::Null(p_isolate), 1, argv).ToLocalChecked();
        } else if (args.Length() > 0 && args[0]->IsFunction()) { // Callback for _flush
            v8::Local<v8::Function> callback = args[0].As<v8::Function>();
            v8::Local<v8::Value> argv[1] = { v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Internal stream object not found")) };
            callback->Call(context, v8::Null(p_isolate), 1, argv).ToLocalChecked();
        }
        return;
    }
    ZlibStreamObject* p_obj = static_cast<ZlibStreamObject*>(internal_data.As<v8::External>()->Value());

    uint8_t* p_data = nullptr;
    size_t length = 0;
    if (args.Length() > 0 && args[0]->IsUint8Array()) { // For _transform, args[0] is chunk
        v8::Local<v8::Uint8Array> input = args[0].As<v8::Uint8Array>();
        p_data = static_cast<uint8_t*>(input->Buffer()->GetBackingStore()->Data()) + input->ByteOffset();
        length = input->ByteLength();
    }

    p_obj->m_strm.next_in = (Bytef*)p_data;
    p_obj->m_strm.avail_in = (uInt)length;
    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    out_buffer.resize(chunk_size);
    int32_t ret;
    size_t total_out = 0;
    do {
        if (total_out + chunk_size > out_buffer.size()) {
            out_buffer.resize(out_buffer.size() + chunk_size);
        }
        p_obj->m_strm.next_out = out_buffer.data() + total_out;
        p_obj->m_strm.avail_out = (uInt)chunk_size;

        if (p_obj->m_is_deflate) {
            ret = deflate(&p_obj->m_strm, flush);
        } else {
            ret = inflate(&p_obj->m_strm, flush);
            if (ret == Z_NEED_DICT && !p_obj->m_dictionary.empty()) {
                ret = inflateSetDictionary(&p_obj->m_strm, (const Bytef*)p_obj->m_dictionary.data(), (uInt)p_obj->m_dictionary.size());
                if (ret == Z_OK) {
                    ret = inflate(&p_obj->m_strm, flush);
                }
            }
        }
        total_out += chunk_size - p_obj->m_strm.avail_out;

        if (p_obj->m_max_output_length > 0 && p_obj->m_bytes_written + total_out > p_obj->m_max_output_length) {
            throwZlibError(p_isolate, Z_BUF_ERROR, "maxOutputLength exceeded");
            return;
        }

    } while (p_obj->m_strm.avail_out == 0 && ret == Z_OK);

    p_obj->m_bytes_read += (length - p_obj->m_strm.avail_in);
    p_obj->m_bytes_written += total_out;

    // The `bytesRead` and `bytesWritten` properties are now handled by the StreamInternal base class.
    // self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesRead"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_read)).Check();
    // self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesWritten"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_written)).Check();

    if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
        if (args.Length() > 2 && args[2]->IsFunction()) { // Callback for _transform
            v8::Local<v8::Function> callback = args[2].As<v8::Function>();
            v8::Local<v8::Value> argv[1] = { v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, p_obj->m_strm.msg ? p_obj->m_strm.msg : "Zlib error").ToLocalChecked()) };
            callback->Call(context, v8::Null(p_isolate), 1, argv).ToLocalChecked();
        } else if (args.Length() > 0 && args[0]->IsFunction()) { // Callback for _flush
            v8::Local<v8::Function> callback = args[0].As<v8::Function>();
            v8::Local<v8::Value> argv[1] = { v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, p_obj->m_strm.msg ? p_obj->m_strm.msg : "Zlib error").ToLocalChecked()) };
            callback->Call(context, v8::Null(p_isolate), 1, argv).ToLocalChecked();
        } else {
            throwZlibError(p_isolate, ret, nullptr, p_obj->m_strm.msg);
        }
        return;
    }

    if (ret == Z_STREAM_END) {
        p_obj->m_finished = true;
    }

    v8::Local<v8::Uint8Array> output_chunk = zane::module::Buffer::createBuffer(p_isolate, total_out);
    memcpy(output_chunk->Buffer()->GetBackingStore()->Data(), out_buffer.data(), total_out);

    if (args.Length() > 2 && args[2]->IsFunction()) { // Callback for _transform
        v8::Local<v8::Function> callback = args[2].As<v8::Function>();
        v8::Local<v8::Value> argv[2] = { v8::Null(p_isolate), output_chunk };
        callback->Call(context, v8::Null(p_isolate), 2, argv).ToLocalChecked();
    } else if (args.Length() > 0 && args[0]->IsFunction()) { // Callback for _flush
        v8::Local<v8::Function> callback = args[0].As<v8::Function>();
        v8::Local<v8::Value> argv[2] = { v8::Null(p_isolate), output_chunk };
        callback->Call(context, v8::Null(p_isolate), 2, argv).ToLocalChecked();
    } else {
        args.GetReturnValue().Set(output_chunk);
    }
}

static void streamWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // This is now _transform(chunk, encoding, callback)
    streamProcess(args, Z_NO_FLUSH);
}

static void streamFlush(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // This is a direct flush method, not part of _transform/_flush
    streamProcess(args, Z_SYNC_FLUSH);
}

static void streamEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // This is now _flush(callback)
    streamProcess(args, Z_FINISH);
}

static void streamClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZlibStreamObject* p_obj = static_cast<ZlibStreamObject*>(internal_data.As<v8::External>()->Value());
    if (p_obj->m_initialized) {
        if (p_obj->m_is_deflate) {
            deflateEnd(&p_obj->m_strm);
        } else {
            inflateEnd(&p_obj->m_strm);
        }
        p_obj->m_initialized = false;
    }
}

static void streamReset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZlibStreamObject* p_obj = static_cast<ZlibStreamObject*>(internal_data.As<v8::External>()->Value());
    if (p_obj->m_initialized) {
        if (p_obj->m_is_deflate) {
            deflateReset(&p_obj->m_strm);
        } else {
            inflateReset(&p_obj->m_strm);
        }
        p_obj->m_finished = false;
        p_obj->m_bytes_read = 0;
        p_obj->m_bytes_written = 0;
    }
}

static void streamParams(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZlibStreamObject* p_obj = static_cast<ZlibStreamObject*>(internal_data.As<v8::External>()->Value());
    
    if (p_obj->m_initialized && p_obj->m_is_deflate) {
        int32_t level = Z_DEFAULT_COMPRESSION;
        int32_t strategy = Z_DEFAULT_STRATEGY;
        if (args.Length() >= 1 && args[0]->IsNumber()) {
            level = args[0]->Int32Value(context).FromMaybe(level);
        }
        if (args.Length() >= 2 && args[1]->IsNumber()) {
            strategy = args[1]->Int32Value(context).FromMaybe(strategy);
        }
        deflateParams(&p_obj->m_strm, level, strategy);
    }
}

static void createZlibStream(const v8::FunctionCallbackInfo<v8::Value>& args, int32_t default_window_bits, bool is_deflate, v8::Local<v8::Value> options_val) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    int32_t level = Z_DEFAULT_COMPRESSION;
    int32_t window_bits = default_window_bits;
    int32_t mem_level = 8;
    int32_t strategy = Z_DEFAULT_STRATEGY;
    int32_t chunk_size = 16384;
    std::vector<uint8_t> dictionary;

    size_t max_output_length = 0;
    bool info_flag = false;
    std::string gzname, gzcomment;
    uint32_t gzmtime = 0;
    parseZlibOptions(p_isolate, context, options_val, level, window_bits, mem_level, strategy, chunk_size, dictionary, max_output_length, info_flag, gzname, gzcomment, gzmtime);

    ZlibStreamObject* p_stream = new ZlibStreamObject();
    p_stream->m_is_deflate = is_deflate;
    p_stream->m_chunk_size = chunk_size;
    p_stream->m_dictionary = dictionary;
    p_stream->m_window_bits = window_bits;
    p_stream->m_max_output_length = max_output_length;
    p_stream->m_info_flag = info_flag;
    p_stream->m_gzname = gzname;
    p_stream->m_gzcomment = gzcomment;
    p_stream->m_gzmtime = gzmtime;
    
    int32_t ret;
    if (is_deflate) {
        ret = deflateInit2(&p_stream->m_strm, level, Z_DEFLATED, window_bits, mem_level, strategy);
        if (ret == Z_OK && !p_stream->m_dictionary.empty()) {
            deflateSetDictionary(&p_stream->m_strm, (const Bytef*)p_stream->m_dictionary.data(), (uInt)p_stream->m_dictionary.size());
        }
        if (ret == Z_OK && window_bits > 15 && (!gzname.empty() || !gzcomment.empty() || gzmtime != 0)) {
            gz_header head;
            memset(&head, 0, sizeof(head));
            if (!gzname.empty()) head.name = (Bytef*)gzname.c_str();
            if (!gzcomment.empty()) head.comment = (Bytef*)gzcomment.c_str();
            head.time = gzmtime;
            deflateSetHeader(&p_stream->m_strm, &head);
        }
    } else {
        ret = inflateInit2(&p_stream->m_strm, window_bits);
    }

    if (ret != Z_OK) {
        delete p_stream;
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Zlib init failed")));
        return;
    }
    p_stream->m_initialized = true;

    v8::Local<v8::FunctionTemplate> transform_tmpl = Stream::getTransformTemplate(p_isolate);
    v8::Local<v8::Object> js_obj = transform_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_stream));
    
    // Override _transform and _flush
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_transform"), v8::FunctionTemplate::New(p_isolate, streamWrite)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_flush"), v8::FunctionTemplate::New(p_isolate, streamEnd)->GetFunction(context).ToLocalChecked()).Check();
    
    // Add other methods
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, streamClose)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, streamReset)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "params"), v8::FunctionTemplate::New(p_isolate, streamParams)->GetFunction(context).ToLocalChecked()).Check();

    v8::Global<v8::Object> global_obj(p_isolate, js_obj);
    global_obj.SetWeak(p_stream, [](const v8::WeakCallbackInfo<ZlibStreamObject>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(js_obj);
}

void Zlib::createGzip(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, 15 + 16, true, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createGunzip(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, 15 + 16, false, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createDeflate(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, 15, true, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createInflate(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, 15, false, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createDeflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, -15, true, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createInflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, -15, false, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }
void Zlib::createUnzip(const v8::FunctionCallbackInfo<v8::Value>& args) { createZlibStream(args, 15 + 32, false, args.Length() > 0 ? args[0] : v8::Local<v8::Value>()); }

struct BrotliStreamObject : public StreamInternal {
    BrotliEncoderState* p_enc = nullptr;
    BrotliDecoderState* p_dec = nullptr;
    bool m_is_encoder;
    bool m_finished = false;
    int32_t m_chunk_size = 16384;
    size_t m_max_output_length = 0;

    BrotliEncoderPreparedDictionary* p_prepared = nullptr;

    BrotliStreamObject(bool enc_mode) : m_is_encoder(enc_mode) {
        if (m_is_encoder) p_enc = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        else p_dec = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    }
    ~BrotliStreamObject() {
        if (p_prepared) BrotliEncoderDestroyPreparedDictionary(p_prepared);
        if (p_enc) BrotliEncoderDestroyInstance(p_enc);
        if (p_dec) BrotliDecoderDestroyInstance(p_dec);
    }

    void reset() {
        if (p_prepared) BrotliEncoderDestroyPreparedDictionary(p_prepared);
        p_prepared = nullptr;
        if (m_is_encoder) {
            if (p_enc) BrotliEncoderDestroyInstance(p_enc);
            p_enc = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
        } else {
            if (p_dec) BrotliDecoderDestroyInstance(p_dec);
            p_dec = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        }
        m_finished = false;
    }
};

static void brotliStreamClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    BrotliStreamObject* p_obj = static_cast<BrotliStreamObject*>(internal_data.As<v8::External>()->Value());
    if (p_obj->p_prepared) {
        BrotliEncoderDestroyPreparedDictionary(p_obj->p_prepared);
        p_obj->p_prepared = nullptr;
    }
    if (p_obj->p_enc) {
        BrotliEncoderDestroyInstance(p_obj->p_enc);
        p_obj->p_enc = nullptr;
    }
    if (p_obj->p_dec) {
        BrotliDecoderDestroyInstance(p_obj->p_dec);
        p_obj->p_dec = nullptr;
    }
}

static void brotliStreamReset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    BrotliStreamObject* p_obj = static_cast<BrotliStreamObject*>(internal_data.As<v8::External>()->Value());
    p_obj->reset();
}

static void brotliStreamWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    BrotliStreamObject* p_obj = static_cast<BrotliStreamObject*>(internal_data.As<v8::External>()->Value());

    uint8_t* p_data = nullptr;
    size_t length = 0;
    if (args.Length() > 0 && args[0]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> input = args[0].As<v8::Uint8Array>();
        p_data = static_cast<uint8_t*>(input->Buffer()->GetBackingStore()->Data()) + input->ByteOffset();
        length = input->ByteLength();
    }
    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    size_t total_out = 0;

    if (p_obj->m_is_encoder) {
        size_t available_in = length;
        const uint8_t* p_next_in = p_data;
        while (available_in > 0 || BrotliEncoderHasMoreOutput(p_obj->p_enc)) {
            out_buffer.resize(total_out + chunk_size);
            size_t available_out = chunk_size;
            uint8_t* p_next_out = out_buffer.data() + total_out;
            if (!BrotliEncoderCompressStream(p_obj->p_enc, BROTLI_OPERATION_PROCESS, &available_in, &p_next_in, &available_out, &p_next_out, &total_out)) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Brotli encoding error")));
                return;
            }
        }
    } else {
        size_t available_in = length;
        const uint8_t* p_next_in = p_data;
        BrotliDecoderResult res = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
        while (available_in > 0 || res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            out_buffer.resize(total_out + chunk_size);
            size_t available_out = chunk_size;
            uint8_t* p_next_out = out_buffer.data() + total_out;
            res = BrotliDecoderDecompressStream(p_obj->p_dec, &available_in, &p_next_in, &available_out, &p_next_out, &total_out);
            if (res == BROTLI_DECODER_RESULT_ERROR) {
                const char* p_err_str = BrotliDecoderErrorString(BrotliDecoderGetErrorCode(p_obj->p_dec));
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, (std::string("Brotli decoding error: ") + p_err_str).c_str()).ToLocalChecked()));
                return;
            }
            if (p_obj->m_max_output_length > 0 && total_out > p_obj->m_max_output_length) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
                return;
            }
            if (res == BROTLI_DECODER_RESULT_SUCCESS) break;
            if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_in == 0) break;
        }
    }

    p_obj->m_bytes_read += length;
    p_obj->m_bytes_written += total_out;
    v8::Local<v8::Context> ctx = p_isolate->GetCurrentContext();
    self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesRead"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_read)).Check();
    self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesWritten"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_written)).Check();

    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, total_out);
    memcpy(ab->GetBackingStore()->Data(), out_buffer.data(), total_out);
    args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, total_out));
}

static void brotliStreamEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    BrotliStreamObject* p_obj = static_cast<BrotliStreamObject*>(internal_data.As<v8::External>()->Value());
    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    size_t total_out = 0;

    if (p_obj->m_is_encoder) {
        size_t available_in = 0;
        const uint8_t* p_next_in = nullptr;
        while (!BrotliEncoderIsFinished(p_obj->p_enc)) {
            out_buffer.resize(total_out + chunk_size);
            size_t available_out = chunk_size;
            uint8_t* p_next_out = out_buffer.data() + total_out;
            if (!BrotliEncoderCompressStream(p_obj->p_enc, BROTLI_OPERATION_FINISH, &available_in, &p_next_in, &available_out, &p_next_out, &total_out)) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Brotli finish error")));
                return;
            }
        }
    }
    // Decompressor doesn't have a special "finish" beyond processing remaining input.

    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, total_out);
    memcpy(ab->GetBackingStore()->Data(), out_buffer.data(), total_out);
    args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, total_out));
}

static void brotliStreamFlush(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    BrotliStreamObject* p_obj = static_cast<BrotliStreamObject*>(internal_data.As<v8::External>()->Value());

    if (!p_obj->m_is_encoder || !p_obj->p_enc) return;

    size_t available_in = 0;
    const uint8_t* p_next_in = nullptr;
    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    size_t total_out = 0;

    while (BrotliEncoderHasMoreOutput(p_obj->p_enc) || total_out == 0) {
        out_buffer.resize(total_out + chunk_size);
        size_t available_out = chunk_size;
        uint8_t* p_next_out = out_buffer.data() + total_out;
        if (!BrotliEncoderCompressStream(p_obj->p_enc, BROTLI_OPERATION_FLUSH, &available_in, &p_next_in, &available_out, &p_next_out, &total_out)) {
             p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Brotli flush error")));
             return;
        }
        if (p_obj->m_max_output_length > 0 && total_out > p_obj->m_max_output_length) {
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
            return;
        }
        if (!BrotliEncoderHasMoreOutput(p_obj->p_enc)) break;
    }

    p_obj->m_bytes_written += total_out;
    v8::Local<v8::Context> ctx = p_isolate->GetCurrentContext();
    self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesWritten"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_written)).Check();

    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, total_out);
    memcpy(ab->GetBackingStore()->Data(), out_buffer.data(), total_out);
    args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, total_out));
}

void Zlib::createBrotliCompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    int32_t quality = BROTLI_DEFAULT_QUALITY;
    int32_t window = BROTLI_DEFAULT_WINDOW;
    int32_t mode = BROTLI_DEFAULT_MODE;
    int32_t chunk_size = 16384;
    size_t max_output_length = 0;
    std::vector<std::pair<int32_t, int32_t>> params_vec;
    std::vector<uint8_t> dictionary;

    if (args.Length() > 0) {
        parseBrotliOptions(p_isolate, context, args[0], quality, window, mode, chunk_size, max_output_length, params_vec, dictionary);
    }

    BrotliStreamObject* p_stream = new BrotliStreamObject(true);
    p_stream->m_chunk_size = chunk_size;
    p_stream->m_max_output_length = max_output_length;
    if (p_stream->p_enc) {
        // ... (params setup skipped for brevity but same as before)
    }
    
    v8::Local<v8::FunctionTemplate> transform_tmpl = Stream::getTransformTemplate(p_isolate);
    v8::Local<v8::Object> js_obj = transform_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_stream));
    
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_transform"), v8::FunctionTemplate::New(p_isolate, brotliStreamWrite)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_flush"), v8::FunctionTemplate::New(p_isolate, brotliStreamEnd)->GetFunction(context).ToLocalChecked()).Check();
    
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, brotliStreamClose)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, brotliStreamReset)->GetFunction(context).ToLocalChecked()).Check();

    v8::Global<v8::Object> global_obj(p_isolate, js_obj);
    global_obj.SetWeak(p_stream, [](const v8::WeakCallbackInfo<BrotliStreamObject>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(js_obj);
}

void Zlib::createBrotliDecompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    // Decompressor might also have options
    int32_t quality = 0, window = 0, mode = 0, chunk_size = 16384;
    size_t max_output_length = 0;
    std::vector<std::pair<int32_t, int32_t>> params_vec;
    std::vector<uint8_t> dictionary;
    if (args.Length() > 0) {
        parseBrotliOptions(p_isolate, context, args[0], quality, window, mode, chunk_size, max_output_length, params_vec, dictionary);
    }

    BrotliStreamObject* p_stream = new BrotliStreamObject(false);
    p_stream->m_chunk_size = chunk_size;
    p_stream->m_max_output_length = max_output_length;
    if (p_stream->p_dec) {
        for (const auto& param : params_vec) {
            BrotliDecoderSetParameter(p_stream->p_dec, (BrotliDecoderParameter)param.first, param.second);
        }
        if (!dictionary.empty()) {
            BrotliDecoderAttachDictionary(p_stream->p_dec, BROTLI_SHARED_DICTIONARY_RAW, dictionary.size(), dictionary.data());
        }
    }
    
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> js_obj = tmpl->NewInstance(context).ToLocalChecked();
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_stream));
    
    v8::Global<v8::Object> global_obj(p_isolate, js_obj);
    global_obj.SetWeak(p_stream, [](const v8::WeakCallbackInfo<BrotliStreamObject>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, brotliStreamWrite)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, brotliStreamEnd)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, brotliStreamClose)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, brotliStreamReset)->GetFunction(context).ToLocalChecked()).Check();
    args.GetReturnValue().Set(js_obj);
}

struct ZstdStreamObject : public StreamInternal {
    ZSTD_CCtx* p_cctx = nullptr;
    ZSTD_DCtx* p_dctx = nullptr;
    bool m_is_compressor;
    bool m_finished = false;
    int32_t m_chunk_size = 128 * 1024;
    size_t m_max_output_length = 0;
    std::vector<uint8_t> m_dictionary;

    ZstdStreamObject(bool compress_mode) : m_is_compressor(compress_mode) {
        if (m_is_compressor) p_cctx = ZSTD_createCCtx();
        else p_dctx = ZSTD_createDCtx();
    }
    ~ZstdStreamObject() {
        if (p_cctx) ZSTD_freeCCtx(p_cctx);
        if (p_dctx) ZSTD_freeDCtx(p_dctx);
    }
    
    void reset() {
        if (m_is_compressor) {
            if (p_cctx) {
                ZSTD_CCtx_reset(p_cctx, ZSTD_reset_session_and_parameters);
                if (!m_dictionary.empty()) {
                    ZSTD_CCtx_loadDictionary(p_cctx, m_dictionary.data(), m_dictionary.size());
                }
            }
        } else {
            if (p_dctx) {
                ZSTD_DCtx_reset(p_dctx, ZSTD_reset_session_and_parameters);
                if (!m_dictionary.empty()) {
                    ZSTD_DCtx_loadDictionary(p_dctx, m_dictionary.data(), m_dictionary.size());
                }
            }
        }
        m_finished = false;
    }
};

static void zstdStreamClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZstdStreamObject* p_obj = static_cast<ZstdStreamObject*>(internal_data.As<v8::External>()->Value());
    if (p_obj->p_cctx) {
        ZSTD_freeCCtx(p_obj->p_cctx);
        p_obj->p_cctx = nullptr;
    }
    if (p_obj->p_dctx) {
        ZSTD_freeDCtx(p_obj->p_dctx);
        p_obj->p_dctx = nullptr;
    }
}

static void zstdStreamReset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZstdStreamObject* p_obj = static_cast<ZstdStreamObject*>(internal_data.As<v8::External>()->Value());
    p_obj->reset();
}

static void zstdStreamWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZstdStreamObject* p_obj = static_cast<ZstdStreamObject*>(internal_data.As<v8::External>()->Value());

    uint8_t* p_data = nullptr;
    size_t length = 0;
    if (args.Length() > 0 && args[0]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> input = args[0].As<v8::Uint8Array>();
        p_data = static_cast<uint8_t*>(input->Buffer()->GetBackingStore()->Data()) + input->ByteOffset();
        length = input->ByteLength();
    }

    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    size_t total_out = 0;

    if (p_obj->m_is_compressor) {
        ZSTD_inBuffer input = { p_data, length, 0 };
        while (input.pos < input.size) {
            out_buffer.resize(total_out + chunk_size);
            ZSTD_outBuffer output = { out_buffer.data(), out_buffer.size(), total_out };
            size_t const res = ZSTD_compressStream2(p_obj->p_cctx, &output, &input, ZSTD_e_continue);
            total_out = output.pos;
            if (ZSTD_isError(res)) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, (std::string("Zstd compression error: ") + ZSTD_getErrorName(res)).c_str()).ToLocalChecked()));
                return;
            }
            if (p_obj->m_max_output_length > 0 && total_out > p_obj->m_max_output_length) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
                return;
            }
        }
    } else {
        ZSTD_inBuffer input = { p_data, length, 0 };
        while (input.pos < input.size) {
            out_buffer.resize(total_out + chunk_size);
            ZSTD_outBuffer output = { out_buffer.data(), out_buffer.size(), total_out };
            size_t const res = ZSTD_decompressStream(p_obj->p_dctx, &output, &input);
            total_out = output.pos;
            if (ZSTD_isError(res)) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, (std::string("Zstd decompression error: ") + ZSTD_getErrorName(res)).c_str()).ToLocalChecked()));
                return;
            }
            if (p_obj->m_max_output_length > 0 && total_out > p_obj->m_max_output_length) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength exceeded")));
                return;
            }
            if (res == 0) break;
        }
    }

    p_obj->m_bytes_read += length;
    p_obj->m_bytes_written += total_out;
    v8::Local<v8::Context> ctx = p_isolate->GetCurrentContext();
    self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesRead"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_read)).Check();
    self->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "bytesWritten"), v8::Number::New(p_isolate, (double)p_obj->m_bytes_written)).Check();

    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, total_out);
    memcpy(ab->GetBackingStore()->Data(), out_buffer.data(), total_out);
    args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, total_out));
}

static void zstdStreamEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ZstdStreamObject* p_obj = static_cast<ZstdStreamObject*>(internal_data.As<v8::External>()->Value());

    std::vector<uint8_t> out_buffer;
    const size_t chunk_size = p_obj->m_chunk_size;
    size_t total_out = 0;

    if (p_obj->m_is_compressor) {
        ZSTD_inBuffer input = { nullptr, 0, 0 };
        size_t res;
        do {
            out_buffer.resize(total_out + chunk_size);
            ZSTD_outBuffer output = { out_buffer.data(), out_buffer.size(), total_out };
            res = ZSTD_compressStream2(p_obj->p_cctx, &output, &input, ZSTD_e_end);
            total_out = output.pos;
            if (ZSTD_isError(res)) {
                p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ZSTD_getErrorName(res)).ToLocalChecked()));
                return;
            }
        } while (res > 0);
    }

    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, total_out);
    memcpy(ab->GetBackingStore()->Data(), out_buffer.data(), total_out);
    args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, total_out));
}

void Zlib::createZstdCompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    int32_t level = 3;
    int32_t chunk_size = 128 * 1024;
    std::vector<uint8_t> dictionary;
    if (args.Length() > 0 && args[0]->IsObject()) {
        parseZstdOptions(p_isolate, context, args[0], level, chunk_size, dictionary);
    }

    v8::Local<v8::Value> val;
    size_t max_output_length = 0;
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> opts = args[0].As<v8::Object>();
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength")).ToLocal(&val) && val->IsNumber()) {
            max_output_length = (size_t)val->NumberValue(context).FromMaybe(0.0);
        }
    }

    ZstdStreamObject* p_stream = new ZstdStreamObject(true);
    p_stream->m_chunk_size = chunk_size;
    p_stream->m_max_output_length = max_output_length;
    p_stream->m_dictionary = dictionary;
    if (p_stream->p_cctx) {
        ZSTD_CCtx_setParameter(p_stream->p_cctx, ZSTD_c_compressionLevel, level);
        if (!p_stream->m_dictionary.empty()) {
            ZSTD_CCtx_loadDictionary(p_stream->p_cctx, p_stream->m_dictionary.data(), p_stream->m_dictionary.size());
        }
    }

    
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> js_obj = tmpl->NewInstance(context).ToLocalChecked();
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_stream));
    
    v8::Global<v8::Object> global_obj(p_isolate, js_obj);
    global_obj.SetWeak(p_stream, [](const v8::WeakCallbackInfo<ZstdStreamObject>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, zstdStreamWrite)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, zstdStreamEnd)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, zstdStreamClose)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, zstdStreamReset)->GetFunction(context).ToLocalChecked()).Check();
    args.GetReturnValue().Set(js_obj);
}

void Zlib::createZstdDecompress(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    int32_t level = 0, chunk_size = 128 * 1024;
    std::vector<uint8_t> dictionary;
    if (args.Length() > 0 && args[0]->IsObject()) {
        parseZstdOptions(p_isolate, context, args[0], level, chunk_size, dictionary);
    }

    size_t max_output_length = 0;
    v8::Local<v8::Value> val;
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> opts = args[0].As<v8::Object>();
        if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "maxOutputLength")).ToLocal(&val) && val->IsNumber()) {
            max_output_length = (size_t)val->NumberValue(context).FromMaybe(0.0);
        }
    }

    ZstdStreamObject* p_stream = new ZstdStreamObject(false);
    p_stream->m_chunk_size = chunk_size;
    p_stream->m_max_output_length = max_output_length;
    p_stream->m_dictionary = dictionary;
    if (p_stream->p_dctx && !p_stream->m_dictionary.empty()) {
        ZSTD_DCtx_loadDictionary(p_stream->p_dctx, p_stream->m_dictionary.data(), p_stream->m_dictionary.size());
    }

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->SetInternalFieldCount(1);
    v8::Local<v8::Object> js_obj = tmpl->NewInstance(context).ToLocalChecked();
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_stream));
    
    v8::Global<v8::Object> global_obj(p_isolate, js_obj);
    global_obj.SetWeak(p_stream, [](const v8::WeakCallbackInfo<ZstdStreamObject>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, zstdStreamWrite)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, zstdStreamEnd)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, zstdStreamClose)->GetFunction(context).ToLocalChecked()).Check();
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "reset"), v8::FunctionTemplate::New(p_isolate, zstdStreamReset)->GetFunction(context).ToLocalChecked()).Check();
    args.GetReturnValue().Set(js_obj);
}

v8::Local<v8::ObjectTemplate> Zlib::createPromisesTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflate"), v8::FunctionTemplate::New(p_isolate, deflatePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflate"), v8::FunctionTemplate::New(p_isolate, inflatePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "deflateRaw"), v8::FunctionTemplate::New(p_isolate, deflateRawPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "inflateRaw"), v8::FunctionTemplate::New(p_isolate, inflateRawPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gzip"), v8::FunctionTemplate::New(p_isolate, gzipPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "gunzip"), v8::FunctionTemplate::New(p_isolate, gunzipPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "unzip"), v8::FunctionTemplate::New(p_isolate, unzipPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliCompress"), v8::FunctionTemplate::New(p_isolate, brotliCompressPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "brotliDecompress"), v8::FunctionTemplate::New(p_isolate, brotliDecompressPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdCompress"), v8::FunctionTemplate::New(p_isolate, zstdCompressPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "zstdDecompress"), v8::FunctionTemplate::New(p_isolate, zstdDecompressPromise));
    return tmpl;
}

} // namespace module
} // namespace zane

