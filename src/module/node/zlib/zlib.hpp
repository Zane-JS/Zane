#ifndef Z8_MODULE_ZLIB_H
#define Z8_MODULE_ZLIB_H

#include "v8.h"

namespace z8 {
namespace module {

class Zlib {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    static void deflateSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflateSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void deflateRawSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflateRawSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gzipSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gunzipSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unzipSync(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void deflate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void deflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gzip(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gunzip(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unzip(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void brotliCompressSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void brotliDecompressSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void brotliCompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void brotliDecompress(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void crc32(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void adler32(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void createGzip(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createGunzip(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createDeflate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createInflate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createDeflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createInflateRaw(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createUnzip(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createBrotliCompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createBrotliDecompress(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void zstdCompressSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void zstdDecompressSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void zstdCompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void zstdDecompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createZstdCompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createZstdDecompress(const v8::FunctionCallbackInfo<v8::Value>& args);
    static v8::Local<v8::ObjectTemplate> createPromisesTemplate(v8::Isolate* p_isolate);

    static void deflatePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflatePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void deflateRawPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void inflateRawPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void gunzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unzipPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void brotliCompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void brotliDecompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void zstdCompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void zstdDecompressPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_ZLIB_H
