#ifndef Z8_MODULE_EVENTS_H
#define Z8_MODULE_EVENTS_H

#include "v8.h"

namespace z8 {
namespace module {

class Events {
  private:
    static v8::Persistent<v8::FunctionTemplate> m_ee_tmpl;

  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> getEventEmitterTemplate(v8::Isolate* p_isolate);

    // Static utilities
    static void once(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void on(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void listenerCount(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getEventListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void getMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void addAbortListener(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void bubbles(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Event, CustomEvent and EventTarget classes
    static v8::Local<v8::FunctionTemplate> createEventTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> createCustomEventTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> event_tmpl);
    static v8::Local<v8::FunctionTemplate> createEventTargetTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::FunctionTemplate> createNodeEventTargetTemplate(v8::Isolate* p_isolate);

    // Static utilities
    static void stopPropagation(const v8::FunctionCallbackInfo<v8::Value>& args);

    // EventEmitter class
    static v8::Local<v8::FunctionTemplate> createEventEmitterTemplate(v8::Isolate* p_isolate);
    
    // EventEmitter prototype methods
    static void eeConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);
    static bool m_using_domains;
    static void eeOn(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeOnce(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeEmit(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeRemoveListener(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeRemoveAllListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeSetMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeGetMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeRawListeners(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeListenerCount(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eePrependListener(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eePrependOnceListener(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void eeEventNames(const v8::FunctionCallbackInfo<v8::Value>& args);
    
    // EventEmitterAsyncResource class
    static v8::Local<v8::FunctionTemplate> createEventEmitterAsyncResourceTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl);
    static void eeAsyncResourceConstructor(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Default max listeners and capture rejections storage
    static int32_t m_default_max_listeners;
    static bool m_default_capture_rejections;

    static void staticGetDefaultMaxListeners(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void staticSetDefaultMaxListeners(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    
    static void staticGetDefaultCaptureRejections(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);
    static void staticSetDefaultCaptureRejections(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info);
    
    static void eeInit(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif // Z8_MODULE_EVENTS_H
