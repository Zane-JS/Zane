#ifndef Z8_MODULE_STREAM_H
#define Z8_MODULE_STREAM_H

#include "v8.h"
#include "../events/events.h"
#include <vector> // Added for std::vector
#include <cstdint>

namespace z8 {
namespace module {

// Internal state for C++ backed streams
struct StreamInternal {
    virtual ~StreamInternal() = default;
    bool m_is_readable = false;
    bool m_is_writable = false;
    bool m_reading = false;
    bool m_writing = false;
    bool m_ended = false;
    bool m_destroyed = false;
    bool m_paused = true;
    bool m_flowing = false;
    bool m_corked = false;
    bool m_finished = false;
    bool m_errored = false;
    bool m_closed = false;
    bool m_allow_half_open = true;
    uint32_t m_high_water_mark = 16 * 1024; // 16KB default
    uint32_t m_cork_count = 0;
    std::vector<uint8_t> m_buffer;
    uint64_t m_bytes_read = 0;
    uint64_t m_bytes_written = 0;
};

class Stream {
  private:
    static v8::Persistent<v8::FunctionTemplate> m_readable_tmpl;
    static v8::Persistent<v8::FunctionTemplate> m_writable_tmpl;
    static v8::Persistent<v8::FunctionTemplate> m_duplex_tmpl;
    static v8::Persistent<v8::FunctionTemplate> m_transform_tmpl;
    static v8::Persistent<v8::FunctionTemplate> m_passthrough_tmpl;

  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);

    // Stream base class methods
    static void streamConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void streamPipe(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void streamUnpipe(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Readable methods
    static v8::Local<v8::FunctionTemplate> createReadableTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl);
    static void readableConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableRead(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readablePush(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readablePause(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableResume(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableDestroy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableSetEncoding(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableFrom(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableIsPaused(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableUnshift(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableWrap(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Readable collection methods
    static void readableMap(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableFilter(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableForEach(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableToArray(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableSome(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableFind(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableEvery(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableFlatMap(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableDrop(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableTake(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readableReduce(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Readable property getters
    static void getReadableProperty(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableFlowing(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableLength(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableEncoding(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableEnded(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableObjectMode(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableClosed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableDestroyed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getReadableErrored(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Helpers for other modules
    static v8::Local<v8::FunctionTemplate> getReadableTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> getWritableTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> getDuplexTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> getTransformTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> getPassThroughTemplate(v8::Isolate* p_isolate);

    // Writable methods
    static v8::Local<v8::FunctionTemplate> createWritableTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl);
    static void writableConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableEnd(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableDestroy(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableCork(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableUncork(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writableSetDefaultEncoding(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Writable property getters
    static void getWritableProperty(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableLength(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableObjectMode(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableCorked(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableEnded(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableFinished(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableNeedDrain(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableClosed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableDestroyed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getWritableErrored(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Duplex and Transform
    static v8::Local<v8::FunctionTemplate> createDuplexTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> readable_tmpl, v8::Local<v8::FunctionTemplate> writable_tmpl);
    static v8::Local<v8::FunctionTemplate> createTransformTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> duplex_tmpl);
    static v8::Local<v8::FunctionTemplate> createPassThroughTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> transform_tmpl);
    static void duplexConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void transformConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void passThroughConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void transformTransform(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void transformFlush(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void transformWrite(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void duplexFrom(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Internal Helpers
    static void pipeline(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void finished(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void compose(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void addAbortSignal(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getDefaultHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setDefaultHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Utilities
    static void isErrored(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isReadable(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void isDisturbed(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void destroy(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // Promises API
    static void pipelinePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void finishedPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_STREAM_H
