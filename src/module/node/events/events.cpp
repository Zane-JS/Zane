#include "events.hpp"
#include "task_queue.hpp" // Added for Task and TaskQueue
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <iostream> // Added for std::cerr and std::endl

namespace zane {
namespace module {

int32_t Events::m_default_max_listeners = 10;
bool Events::m_default_capture_rejections = false;
bool Events::m_using_domains = false;
v8::Persistent<v8::FunctionTemplate> Events::m_ee_tmpl;

v8::Local<v8::ObjectTemplate> Events::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    v8::Local<v8::FunctionTemplate> ee_tmpl = getEventEmitterTemplate(p_isolate);

    // Static properties
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "usingDomains"), v8::Boolean::New(p_isolate, m_using_domains));

    // The module object itself is just a collection of these
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EventEmitter"), ee_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "default"), ee_tmpl);

    v8::Local<v8::FunctionTemplate> ee_async_tmpl = createEventEmitterAsyncResourceTemplate(p_isolate, ee_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EventEmitterAsyncResource"), ee_async_tmpl);
    
    // Node v24: Static properties on events module
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "usingDomains"), v8::Boolean::New(p_isolate, m_using_domains));
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "defaultMaxListeners"), staticGetDefaultMaxListeners, staticSetDefaultMaxListeners);
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "captureRejections"), staticGetDefaultCaptureRejections, staticSetDefaultCaptureRejections);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::FunctionTemplate::New(p_isolate, once));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "on"), v8::FunctionTemplate::New(p_isolate, on));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "listenerCount"), v8::FunctionTemplate::New(p_isolate, listenerCount));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getEventListeners"), v8::FunctionTemplate::New(p_isolate, getEventListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getMaxListeners"), v8::FunctionTemplate::New(p_isolate, getMaxListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "setMaxListeners"), v8::FunctionTemplate::New(p_isolate, setMaxListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "addAbortListener"), v8::FunctionTemplate::New(p_isolate, addAbortListener));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "stopPropagation"), v8::FunctionTemplate::New(p_isolate, stopPropagation));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "bubbles"), v8::FunctionTemplate::New(p_isolate, bubbles));
    
    v8::Local<v8::FunctionTemplate> event_tmpl = createEventTemplate(p_isolate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Event"), event_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "CustomEvent"), createCustomEventTemplate(p_isolate, event_tmpl));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EventTarget"), createEventTargetTemplate(p_isolate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "NodeEventTarget"), createNodeEventTargetTemplate(p_isolate));

    v8::Local<v8::Symbol> error_monitor_sym = v8::Symbol::For(
        p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "events.errorMonitor"));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "errorMonitor"), error_monitor_sym);

    v8::Local<v8::Symbol> capture_rejection_sym = v8::Symbol::For(
        p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "nodejs.rejection"));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "captureRejectionSymbol"), capture_rejection_sym);

    // captureRejections flag (global default)
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "captureRejections"),
                                staticGetDefaultCaptureRejections, staticSetDefaultCaptureRejections);

    return tmpl;
}

v8::Local<v8::FunctionTemplate> Events::createEventEmitterTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, eeConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "EventEmitter"));
    
    // Prototype methods
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "on"), v8::FunctionTemplate::New(p_isolate, eeOn));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "addListener"), v8::FunctionTemplate::New(p_isolate, eeOn));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::FunctionTemplate::New(p_isolate, eeOnce));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "emit"), v8::FunctionTemplate::New(p_isolate, eeEmit));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), v8::FunctionTemplate::New(p_isolate, eeRemoveListener));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "off"), v8::FunctionTemplate::New(p_isolate, eeRemoveListener));
    v8::Local<v8::FunctionTemplate> ee_remove_all_listeners_tmpl = v8::FunctionTemplate::New(p_isolate, eeRemoveAllListeners);
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeAllListeners"), ee_remove_all_listeners_tmpl);
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "setMaxListeners"), v8::FunctionTemplate::New(p_isolate, eeSetMaxListeners));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "getMaxListeners"), v8::FunctionTemplate::New(p_isolate, eeGetMaxListeners));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "listeners"), v8::FunctionTemplate::New(p_isolate, eeListeners));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "rawListeners"), v8::FunctionTemplate::New(p_isolate, eeRawListeners));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "listenerCount"), v8::FunctionTemplate::New(p_isolate, eeListenerCount));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "prependListener"), v8::FunctionTemplate::New(p_isolate, eePrependListener));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "prependOnceListener"), v8::FunctionTemplate::New(p_isolate, eePrependOnceListener));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "eventNames"), v8::FunctionTemplate::New(p_isolate, eeEventNames));

    // Static properties on the constructor
    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "defaultMaxListeners"), staticGetDefaultMaxListeners, staticSetDefaultMaxListeners);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EventEmitter"), tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "usingDomains"), v8::Boolean::New(p_isolate, m_using_domains));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::FunctionTemplate::New(p_isolate, once));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "on"), v8::FunctionTemplate::New(p_isolate, on));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "listenerCount"), v8::FunctionTemplate::New(p_isolate, listenerCount));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getEventListeners"), v8::FunctionTemplate::New(p_isolate, getEventListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getMaxListeners"), v8::FunctionTemplate::New(p_isolate, getMaxListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "setMaxListeners"), v8::FunctionTemplate::New(p_isolate, setMaxListeners));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "addAbortListener"), v8::FunctionTemplate::New(p_isolate, addAbortListener));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "stopPropagation"), v8::FunctionTemplate::New(p_isolate, stopPropagation));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "bubbles"), v8::FunctionTemplate::New(p_isolate, bubbles));
    
    // Static properties on the constructor (continued)
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "usingDomains"), v8::Boolean::New(p_isolate, m_using_domains));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "init"), v8::FunctionTemplate::New(p_isolate, eeInit));
    
    v8::Local<v8::FunctionTemplate> event_tmpl_ee = createEventTemplate(p_isolate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Event"), event_tmpl_ee);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "CustomEvent"), createCustomEventTemplate(p_isolate, event_tmpl_ee));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EventTarget"), createEventTargetTemplate(p_isolate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "NodeEventTarget"), createNodeEventTargetTemplate(p_isolate));

    // Static properties on the constructor
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "usingDomains"), v8::Boolean::New(p_isolate, m_using_domains));

    v8::Local<v8::Symbol> error_monitor_sym_ee = v8::Symbol::For(
        p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "events.errorMonitor"));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "errorMonitor"), error_monitor_sym_ee);

    // Symbols — same as module-level exports
    v8::Local<v8::Symbol> capture_rejection_sym = v8::Symbol::For(
        p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "nodejs.rejection"));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "captureRejectionSymbol"), capture_rejection_sym);

    // Explicit Resource Management (Symbol.dispose and Symbol.asyncDispose)
    v8::Local<v8::Symbol> dispose_sym = v8::Symbol::GetDispose(p_isolate);
    v8::Local<v8::Symbol> async_dispose_sym = v8::Symbol::GetAsyncDispose(p_isolate);
    
    // Use the template defined earlier in the prototype section
    proto->Set(dispose_sym, ee_remove_all_listeners_tmpl);
    proto->Set(async_dispose_sym, ee_remove_all_listeners_tmpl);

    tmpl->SetNativeDataProperty(v8::String::NewFromUtf8Literal(p_isolate, "captureRejections"),
                                staticGetDefaultCaptureRejections, staticSetDefaultCaptureRejections);

    return tmpl;
}

// EventEmitter Implementation

// Data structure to hold V8 Global handles for the task
struct ListenerTaskData {
    v8::Global<v8::Object> m_emitter;
    v8::Global<v8::Function> m_listener;
    std::vector<v8::Global<v8::Value>> m_argv;

    // Constructor to initialize Global handles
    ListenerTaskData(v8::Isolate* p_isolate, v8::Local<v8::Object> emitter, v8::Local<v8::Function> listener, const std::vector<v8::Local<v8::Value>>& argv)
        : m_emitter(p_isolate, emitter), m_listener(p_isolate, listener) {
        for (const auto& arg : argv) {
            m_argv.emplace_back(p_isolate, arg);
        }
    }

    // Destructor to dispose Global handles
    ~ListenerTaskData() {
        m_emitter.Reset();
        m_listener.Reset();
        for (auto& arg : m_argv) {
            arg.Reset();
        }
    }
};

// The runner function for the TaskQueue
static void ListenerTaskRunner(v8::Isolate* p_isolate, v8::Local<v8::Context> context, zane::Task* p_task) {
    v8::HandleScope handle_scope(p_isolate);
    v8::Context::Scope context_scope(context);

    ListenerTaskData* p_data = static_cast<ListenerTaskData*>(p_task->p_data);

    v8::Local<v8::Object> emitter = p_data->m_emitter.Get(p_isolate);
    v8::Local<v8::Function> listener = p_data->m_listener.Get(p_isolate);
    
    std::vector<v8::Local<v8::Value>> local_argv;
    for (const auto& global_arg : p_data->m_argv) {
        local_argv.push_back(global_arg.Get(p_isolate));
    }

    v8::TryCatch try_catch(p_isolate);
    (void)listener->Call(context, emitter, static_cast<int32_t>(local_argv.size()), local_argv.data());

    if (try_catch.HasCaught()) {
        // Report exception to stderr
        v8::String::Utf8Value exception(p_isolate, try_catch.Exception());
        std::cerr << "Uncaught exception in event listener: " << (*exception ? *exception : "unknown") << std::endl;
    }

    delete p_data; // Clean up the data
}

// Helper to enqueue a listener call
static void enqueueListenerCall(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Object> emitter, v8::Local<v8::Function> listener, const std::vector<v8::Local<v8::Value>>& argv) {
    ListenerTaskData* p_data = new ListenerTaskData(p_isolate, emitter, listener, argv);
    zane::Task* p_task = new zane::Task();
    p_task->p_data = p_data;
    p_task->m_runner = ListenerTaskRunner;
    p_task->m_is_promise = false; // This is not a promise-based task
    zane::TaskQueue::getInstance().enqueue(p_task);
}

void Events::eeConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount"), v8::Integer::New(p_isolate, 0));
    v8::Local<v8::Object> events_obj = v8::Object::New(p_isolate);
    self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_events"), events_obj).Check();
    self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_maxListeners"), v8::Undefined(p_isolate)).Check();
    
    bool capture_rejections = m_default_capture_rejections;
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> cr;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "captureRejections")).ToLocal(&cr)) {
            capture_rejections = cr->BooleanValue(p_isolate);
        }
    }
    self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "captureRejections"), v8::Boolean::New(p_isolate, capture_rejections)).Check();
}

v8::Local<v8::FunctionTemplate> Events::createEventEmitterAsyncResourceTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, eeAsyncResourceConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "EventEmitterAsyncResource"));
    
    // Inherit from EventEmitter
    tmpl->Inherit(ee_tmpl);

    return tmpl;
}

void Events::eeAsyncResourceConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    // Call base constructor
    eeConstructor(args);

    // Initialise async resource properties
    v8::Local<v8::String> name = v8::String::NewFromUtf8Literal(p_isolate, "EventEmitterAsyncResource");
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> n_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "name")).ToLocal(&n_val) && n_val->IsString()) {
            name = n_val.As<v8::String>();
        }
    }

    // Since we don't have full async_hooks, we just store the name
    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "asyncResource"), v8::Object::New(p_isolate));
}

static void addListenerInternal(v8::Isolate* p_isolate, v8::Local<v8::Object> self, v8::Local<v8::Value> event, v8::Local<v8::Value> listener, bool prepend) {
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    v8::Local<v8::Value> events_val;
    v8::Local<v8::Object> events_obj;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        events_obj = v8::Object::New(p_isolate);
        self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_events"), events_obj).Check();
    } else {
        events_obj = events_val.As<v8::Object>();
    }

    // Emit 'newListener'
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "newListener"), event, listener };
        // Node compatibility: Ensure 'newListener' is emitted BEFORE adding to internal storage
        (void)emit_val.As<v8::Function>()->Call(context, self, 3, argv);
    }

    v8::Local<v8::Value> existing;
    bool is_new = false;
    if (!events_obj->Get(context, event).ToLocal(&existing) || existing->IsUndefined()) {
        events_obj->Set(context, event, listener).Check();
        is_new = true;
    } else if (existing->IsFunction()) {
        v8::Local<v8::Array> arr = v8::Array::New(p_isolate, 2);
        if (prepend) {
            arr->Set(context, 0, listener).Check();
            arr->Set(context, 1, existing).Check();
        } else {
            arr->Set(context, 0, existing).Check();
            arr->Set(context, 1, listener).Check();
        }
        events_obj->Set(context, event, arr).Check();
    } else if (existing->IsArray()) {
        v8::Local<v8::Array> arr = existing.As<v8::Array>();
        if (prepend) {
            uint32_t len = arr->Length();
            v8::Local<v8::Array> new_arr = v8::Array::New(p_isolate, len + 1);
            new_arr->Set(context, 0, listener).Check();
            for (uint32_t i = 0; i < len; i++) {
                new_arr->Set(context, i + 1, arr->Get(context, i).ToLocalChecked()).Check();
            }
            events_obj->Set(context, event, new_arr).Check();
        } else {
            arr->Set(context, arr->Length(), listener).Check();
        }
    }

    if (is_new) {
        v8::Local<v8::Value> count_val;
        int32_t count = 0;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount")).ToLocal(&count_val) && count_val->IsNumber()) {
            count = count_val->Int32Value(context).FromMaybe(0);
        }
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount"), v8::Integer::New(p_isolate, count + 1));
    }

    // Check MaxListeners limit
    v8::Local<v8::Value> max_val;
    int32_t max_listeners = Events::m_default_max_listeners;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_maxListeners")).ToLocal(&max_val) && max_val->IsNumber()) {
        max_listeners = max_val->Int32Value(context).FromMaybe(Events::m_default_max_listeners);
    }

    if (max_listeners > 0) {
        v8::Local<v8::Value> handlers_check;
        if (events_obj->Get(context, event).ToLocal(&handlers_check) && handlers_check->IsArray()) {
            int32_t count = handlers_check.As<v8::Array>()->Length();
            if (count > max_listeners) {
                v8::String::Utf8Value ev_name(p_isolate, event);
                fprintf(stderr, "(node) warning: possible EventEmitter memory leak detected. %d %s listeners added. Use emitter.setMaxListeners() to increase limit\n", count, *ev_name);
            }
        }
    }
}

void Events::eeOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"listener\" argument must be of type function")));
        return;
    }
    addListenerInternal(p_isolate, args.This(), args[0], args[1], false);
    args.GetReturnValue().Set(args.This());
}

void Events::eePrependListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"listener\" argument must be of type function")));
        return;
    }
    addListenerInternal(p_isolate, args.This(), args[0], args[1], true);
    args.GetReturnValue().Set(args.This());
}

// Wrapper for 'once'
struct OnceData {
    v8::Global<v8::Object> m_emitter;
    v8::Global<v8::Value> m_event;
    v8::Global<v8::Function> m_listener;
    v8::Global<v8::Function> m_wrapper;
};

static void onceWrapper(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    OnceData* p_data = static_cast<OnceData*>(v8::Local<v8::External>::Cast(args.Data())->Value());

    v8::Local<v8::Object> emitter = p_data->m_emitter.Get(p_isolate);
    v8::Local<v8::Value> event = p_data->m_event.Get(p_isolate);
    v8::Local<v8::Function> listener = p_data->m_listener.Get(p_isolate);
    v8::Local<v8::Function> wrapper = p_data->m_wrapper.Get(p_isolate);

    // Remove first
    v8::Local<v8::Value> off_val;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeListener")).ToLocal(&off_val) && off_val->IsFunction()) {
        v8::Local<v8::Value> off_argv[] = { event, wrapper };
        (void)off_val.As<v8::Function>()->Call(context, emitter, 2, off_argv);
    }

    // Call listener
    std::vector<v8::Local<v8::Value>> argv;
    for (int32_t i = 0; i < args.Length(); i++) argv.push_back(args[i]);
    listener->Call(context, emitter, (int32_t)argv.size(), argv.data()).FromMaybe(v8::Local<v8::Value>());

    // Cleanup data if needed? Usually V8 handles it if we use Weak callbacks, 
    // but here we might need manual management if we didn't use Weak.
    // For now, let's just use regular globals and accept a small leak if never called.
    // In a real engine, we'd use Weak handles.
}

void Events::eeOnce(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"listener\" argument must be of type function")));
        return;
    }

    auto* p_data = new OnceData{
        v8::Global<v8::Object>(p_isolate, args.This()),
        v8::Global<v8::Value>(p_isolate, args[0]),
        v8::Global<v8::Function>(p_isolate, args[1].As<v8::Function>()),
        v8::Global<v8::Function>()
    };

    v8::Local<v8::Function> wrapper = v8::Function::New(context, onceWrapper, v8::External::New(p_isolate, p_data)).ToLocalChecked();
    p_data->m_wrapper.Reset(p_isolate, wrapper);
    
    // Attach the original listener to the wrapper for 'rawListeners' and 'removeListener'
    wrapper->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "listener"), args[1]).Check();

    addListenerInternal(p_isolate, args.This(), args[0], wrapper, false);
    args.GetReturnValue().Set(args.This());
}

void Events::eePrependOnceListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"listener\" argument must be of type function")));
        return;
    }

    auto* p_data = new OnceData{
        v8::Global<v8::Object>(p_isolate, args.This()),
        v8::Global<v8::Value>(p_isolate, args[0]),
        v8::Global<v8::Function>(p_isolate, args[1].As<v8::Function>()),
        v8::Global<v8::Function>()
    };

    v8::Local<v8::Function> wrapper = v8::Function::New(context, onceWrapper, v8::External::New(p_isolate, p_data)).ToLocalChecked();
    p_data->m_wrapper.Reset(p_isolate, wrapper);
    wrapper->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "listener"), args[1]).Check();

    addListenerInternal(p_isolate, args.This(), args[0], wrapper, true);
    args.GetReturnValue().Set(args.This());
}

static void asyncRejectionHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> data = args.Data().As<v8::Object>();

    v8::Local<v8::Value> emitter_val;
    if (!data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emitter")).ToLocal(&emitter_val)) return;
    v8::Local<v8::Object> emitter = emitter_val.As<v8::Object>();

    v8::Local<v8::Value> reason = args[0];
    v8::Local<v8::Value> event_name;
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "event")).ToLocal(&event_name);

    // Check for custom rejection handler Symbol.for('nodejs.rejection')
    v8::Local<v8::Symbol> rej_sym = v8::Symbol::For(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "nodejs.rejection"));
    v8::Local<v8::Value> handler_val;
    if (emitter->Get(context, rej_sym).ToLocal(&handler_val) && handler_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { reason, event_name };
        (void)handler_val.As<v8::Function>()->Call(context, emitter, 2, argv);
    } else {
        // Default: emit 'error'
        v8::Local<v8::Value> emit_fn;
        if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_fn) && emit_fn->IsFunction()) {
            v8::Local<v8::Value> emit_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), reason };
            (void)emit_fn.As<v8::Function>()->Call(context, emitter, 2, emit_argv);
        }
    }
}

void Events::eeEmit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1) {
        args.GetReturnValue().Set(false);
        return;
    }

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Value> event_name = args[0];

    v8::Local<v8::Value> events_val;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        // Special 'error' handling
        if (event_name->IsString()) {
            v8::String::Utf8Value ev(p_isolate, event_name);
            if (strcmp(*ev, "error") == 0) {
                if (args.Length() > 1) p_isolate->ThrowException(args[1]);
                else p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Unhandled error event")));
                return;
            }
        }
        args.GetReturnValue().Set(false);
        return;
    }

    v8::Local<v8::Object> events_obj = events_val.As<v8::Object>();
    v8::Local<v8::Value> handlers;

    // Support events.errorMonitor
    if (event_name->IsString()) {
        v8::String::Utf8Value ev(p_isolate, event_name);
        if (strcmp(*ev, "error") == 0) {
            v8::Local<v8::Symbol> error_monitor_sym = v8::Symbol::For(
                p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "events.errorMonitor"));
            v8::Local<v8::Value> monitor_handlers;
            if (events_obj->Get(context, error_monitor_sym).ToLocal(&monitor_handlers) && !monitor_handlers->IsUndefined()) {
                std::vector<v8::Local<v8::Value>> monitor_argv;
                for (int32_t i = 1; i < args.Length(); i++) monitor_argv.push_back(args[i]);
                if (monitor_handlers->IsFunction()) {
                    (void)monitor_handlers.As<v8::Function>()->Call(context, self, (int32_t)monitor_argv.size(), monitor_argv.data());
                } else if (monitor_handlers->IsArray()) {
                    v8::Local<v8::Array> m_arr = monitor_handlers.As<v8::Array>();
                    uint32_t m_len = m_arr->Length();
                    for (uint32_t i = 0; i < m_len; i++) {
                        v8::Local<v8::Value> h_val = m_arr->Get(context, i).ToLocalChecked();
                        if (h_val->IsFunction()) (void)h_val.As<v8::Function>()->Call(context, self, (int32_t)monitor_argv.size(), monitor_argv.data());
                    }
                }
            }
        }
    }

    if (!events_obj->Get(context, event_name).ToLocal(&handlers) || handlers->IsUndefined()) {
        if (event_name->IsString()) {
            v8::String::Utf8Value ev(p_isolate, event_name);
            if (strcmp(*ev, "error") == 0) {
                if (args.Length() > 1) p_isolate->ThrowException(args[1]);
                else p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Unhandled error event")));
                return;
            }
        }
        args.GetReturnValue().Set(false);
        return;
    }

    std::vector<v8::Local<v8::Value>> argv;
    for (int32_t i = 1; i < args.Length(); i++) argv.push_back(args[i]);

    v8::Local<v8::Value> cr_val;
    bool capture_rejections = false;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "captureRejections")).ToLocal(&cr_val)) {
        capture_rejections = cr_val->BooleanValue(p_isolate);
    }

    if (handlers->IsFunction()) {
        v8::Local<v8::Function> h = handlers.As<v8::Function>();
        enqueueListenerCall(p_isolate, context, self, h, argv);
    } else if (handlers->IsArray()) {
        v8::Local<v8::Array> arr = handlers.As<v8::Array>();
        uint32_t len = arr->Length();
        // Snapshotted copy to handle mutations during emit
        v8::Local<v8::Array> snapshot = v8::Array::New(p_isolate, len);
        for (uint32_t i = 0; i < len; i++) snapshot->Set(context, i, arr->Get(context, i).ToLocalChecked()).Check();

        for (uint32_t i = 0; i < len; i++) {
            v8::Local<v8::Value> h_val = snapshot->Get(context, i).ToLocalChecked();
            if (h_val->IsFunction()) {
                v8::Local<v8::Function> h = h_val.As<v8::Function>();
                enqueueListenerCall(p_isolate, context, self, h, argv);
            }
        }
    }

    args.GetReturnValue().Set(true);
}

void Events::eeRemoveListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[1]->IsFunction()) {
        args.GetReturnValue().Set(args.This());
        return;
    }

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Value> event = args[0];
    v8::Local<v8::Value> listener = args[1];

    v8::Local<v8::Value> events_val;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(self);
        return;
    }

    v8::Local<v8::Object> events_obj = events_val.As<v8::Object>();
    v8::Local<v8::Value> handlers;
    if (!events_obj->Get(context, event).ToLocal(&handlers) || handlers->IsUndefined()) {
        args.GetReturnValue().Set(self);
        return;
    }

    bool removed = false;
    if (handlers->IsFunction()) {
        v8::Local<v8::Function> h = handlers.As<v8::Function>();
        v8::Local<v8::Value> original = h->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listener")).FromMaybe(v8::Local<v8::Value>());
        if (h->StrictEquals(listener) || (original->IsFunction() && original->StrictEquals(listener))) {
            events_obj->Delete(context, event).Check();
            removed = true;
        }
    } else if (handlers->IsArray()) {
        v8::Local<v8::Array> arr = handlers.As<v8::Array>();
        uint32_t len = arr->Length();
        for (uint32_t i = 0; i < len; i++) {
            v8::Local<v8::Value> h = arr->Get(context, i).ToLocalChecked();
            v8::Local<v8::Value> original;
            if (h->IsObject()) original = h.As<v8::Object>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listener")).FromMaybe(v8::Local<v8::Value>());
            
            if (h->StrictEquals(listener) || (original->IsFunction() && original->StrictEquals(listener))) {
                // Remove from array
                v8::Local<v8::Array> new_arr = v8::Array::New(p_isolate, len - 1);
                for (uint32_t j = 0, k = 0; j < len; j++) {
                    if (j == i) continue;
                    new_arr->Set(context, k++, arr->Get(context, j).ToLocalChecked()).Check();
                }
                if (new_arr->Length() == 1) events_obj->Set(context, event, new_arr->Get(context, 0).ToLocalChecked()).Check();
                else events_obj->Set(context, event, new_arr).Check();
                removed = true;
                break;
            }
        }
    }

    if (removed) {
        // Update _eventsCount if event was completely removed
        v8::Local<v8::Value> check_existing;
        if (!events_obj->Get(context, event).ToLocal(&check_existing) || check_existing->IsUndefined()) {
            v8::Local<v8::Value> count_val;
            int32_t count = 0;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount")).ToLocal(&count_val) && count_val->IsNumber()) {
                count = count_val->Int32Value(context).FromMaybe(0);
            }
            if (count > 0) {
                (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount"), v8::Integer::New(p_isolate, count - 1));
            }
        }

        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), event, listener };
            // Node compatibility: Ensure 'removeListener' is emitted AFTER removal
            (void)emit_val.As<v8::Function>()->Call(context, self, 3, argv);
        }
    }

    args.GetReturnValue().Set(self);
}

void Events::eeRemoveAllListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Value> events_val;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(self);
        return;
    }
    v8::Local<v8::Object> events_obj = events_val.As<v8::Object>();

    v8::Local<v8::Value> emit_val;
    bool has_emit = self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction();

    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::Local<v8::Value> event = args[0];
        v8::Local<v8::Value> handlers;
        if (events_obj->Get(context, event).ToLocal(&handlers) && !handlers->IsUndefined()) {
            if (has_emit) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                if (handlers->IsFunction()) {
                    v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), event, handlers };
                    (void)emit_fn->Call(context, self, 3, argv);
                } else if (handlers->IsArray()) {
                    v8::Local<v8::Array> arr = handlers.As<v8::Array>();
                    uint32_t len = arr->Length();
                    for (uint32_t i = 0; i < len; i++) {
                        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), event, arr->Get(context, i).ToLocalChecked() };
                        (void)emit_fn->Call(context, self, 3, argv);
                    }
                }
            }
            events_obj->Delete(context, event).Check();
            v8::Local<v8::Value> count_val;
            int32_t count = 0;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount")).ToLocal(&count_val) && count_val->IsNumber()) {
                count = count_val->Int32Value(context).FromMaybe(0);
            }
            if (count > 0) {
                (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount"), v8::Integer::New(p_isolate, count - 1));
            }
        }
    } else {
        if (has_emit) {
            v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
            v8::Local<v8::Array> names;
            if (events_obj->GetPropertyNames(context).ToLocal(&names)) {
                for (uint32_t i = 0; i < names->Length(); i++) {
                    v8::Local<v8::Value> event = names->Get(context, i).ToLocalChecked();
                    v8::Local<v8::Value> handlers = events_obj->Get(context, event).ToLocalChecked();
                    if (handlers->IsFunction()) {
                        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), event, handlers };
                        (void)emit_fn->Call(context, self, 3, argv);
                    } else if (handlers->IsArray()) {
                        v8::Local<v8::Array> arr = handlers.As<v8::Array>();
                        for (uint32_t j = 0; j < arr->Length(); j++) {
                            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), event, arr->Get(context, j).ToLocalChecked() };
                            (void)emit_fn->Call(context, self, 3, argv);
                        }
                    }
                }
            }
        }
        self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_events"), v8::Object::New(p_isolate)).Check();
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_eventsCount"), v8::Integer::New(p_isolate, 0));
    }
    args.GetReturnValue().Set(self);
}

void Events::eeSetMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsNumber()) {
         p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"n\" argument must be of type number")));
         return;
    }
    int32_t n = args[0]->Int32Value(context).FromMaybe(-1);
    if (n < 0) {
        p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "The value of \"n\" is out of range. It must be >= 0.")));
        return;
    }
    args.This()->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_maxListeners"), args[0]).Check();
    args.GetReturnValue().Set(args.This());
}

void Events::eeGetMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> val;
    if (args.This()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_maxListeners")).ToLocal(&val) && val->IsNumber()) {
        args.GetReturnValue().Set(val);
    } else {
        args.GetReturnValue().Set(m_default_max_listeners);
    }
}

void Events::eeListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> events_val;
    if (!args.This()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
        return;
    }
    v8::Local<v8::Value> h;
    if (!events_val.As<v8::Object>()->Get(context, args[0]).ToLocal(&h) || h->IsUndefined()) {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
        return;
    }
    if (h->IsFunction()) {
        v8::Local<v8::Array> res = v8::Array::New(p_isolate, 1);
        v8::Local<v8::Value> orig = h.As<v8::Object>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listener")).FromMaybe(h);
        res->Set(context, 0, orig).Check();
        args.GetReturnValue().Set(res);
    } else if (h->IsArray()) {
        v8::Local<v8::Array> arr = h.As<v8::Array>();
        v8::Local<v8::Array> res = v8::Array::New(p_isolate, arr->Length());
        for (uint32_t i = 0; i < arr->Length(); i++) {
            v8::Local<v8::Value> item = arr->Get(context, i).ToLocalChecked();
            v8::Local<v8::Value> orig = item.As<v8::Object>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listener")).FromMaybe(item);
            res->Set(context, i, orig).Check();
        }
        args.GetReturnValue().Set(res);
    }
}

void Events::eeRawListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> events_val;
    if (!args.This()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
        return;
    }
    v8::Local<v8::Value> h;
    if (!events_val.As<v8::Object>()->Get(context, args[0]).ToLocal(&h) || h->IsUndefined()) {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
        return;
    }
    if (h->IsFunction()) {
        v8::Local<v8::Array> res = v8::Array::New(p_isolate, 1);
        res->Set(context, 0, h).Check();
        args.GetReturnValue().Set(res);
    } else if (h->IsArray()) {
        args.GetReturnValue().Set(h);
    }
}

void Events::eeListenerCount(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Value> event_name;

    if (args.Length() == 0) {
        args.GetReturnValue().Set(0);
        return;
    }

    // Node compat: if args[0] is emitter-like and args[1] is eventName, use static behavior
    if (args.Length() >= 2 && args[0]->IsObject() && (args[1]->IsString() || args[1]->IsSymbol())) {
        listenerCount(args);
        return;
    }
    
    event_name = args[0];

    v8::Local<v8::Value> events_val;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(0);
        return;
    }
    v8::Local<v8::Value> h;
    if (!events_val.As<v8::Object>()->Get(context, event_name).ToLocal(&h) || h->IsUndefined()) {
        args.GetReturnValue().Set(0);
        return;
    }
    if (h->IsFunction()) args.GetReturnValue().Set(1);
    else if (h->IsArray()) args.GetReturnValue().Set(static_cast<int32_t>(h.As<v8::Array>()->Length()));
    else args.GetReturnValue().Set(0);
}

void Events::eeEventNames(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> events_val;
    if (!args.This()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_events")).ToLocal(&events_val) || !events_val->IsObject()) {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
        return;
    }
    v8::Local<v8::Array> props;
    if (events_val.As<v8::Object>()->GetPropertyNames(context).ToLocal(&props)) {
        args.GetReturnValue().Set(props);
    } else {
        args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
    }
}

// Static Utilities Implementation

// Helper for events.once resolution
static void onceCleanup(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Object> data) {
    v8::Local<v8::Value> emitter_val, name_val, resolve_wrapper_val, error_wrapper_val, abort_listener_val, signal_val;
    if (!data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emitter")).ToLocal(&emitter_val) || !emitter_val->IsObject()) return;
    v8::Local<v8::Object> emitter = emitter_val.As<v8::Object>();
    
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "name")).ToLocal(&name_val);
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resolveWrapper")).ToLocal(&resolve_wrapper_val);
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "errorWrapper")).ToLocal(&error_wrapper_val);
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "abortListener")).ToLocal(&abort_listener_val);
    (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "signal")).ToLocal(&signal_val);

    v8::Local<v8::Value> remove_fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
        if (!resolve_wrapper_val.IsEmpty() && !resolve_wrapper_val->IsUndefined()) {
            v8::Local<v8::Value> argv[] = { name_val, resolve_wrapper_val };
            (void)remove_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
        }
        if (!error_wrapper_val.IsEmpty() && !error_wrapper_val->IsUndefined()) {
             v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_wrapper_val };
             (void)remove_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
        }
    } else if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeEventListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
         if (!resolve_wrapper_val.IsEmpty() && !resolve_wrapper_val->IsUndefined()) {
            v8::Local<v8::Value> argv[] = { name_val, resolve_wrapper_val };
            (void)remove_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
        }
    }

    if (!signal_val.IsEmpty() && signal_val->IsObject() && !abort_listener_val.IsEmpty() && abort_listener_val->IsFunction()) {
        v8::Local<v8::Object> signal = signal_val.As<v8::Object>();
        v8::Local<v8::Value> r_fn;
        if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeEventListener")).ToLocal(&r_fn) && r_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "abort"), abort_listener_val };
            (void)r_fn.As<v8::Function>()->Call(context, signal, 2, argv);
        }
    }
}

static void onceAbortWrapper(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> data = args.Data().As<v8::Object>();

    v8::Local<v8::Value> resolver_val;
    if (!data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resolver")).ToLocal(&resolver_val)) return;
    v8::Local<v8::Promise::Resolver> resolver = resolver_val.As<v8::Promise::Resolver>();

    v8::Local<v8::Value> signal_val;
    v8::Local<v8::Value> reason;
    if (data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "signal")).ToLocal(&signal_val) && signal_val->IsObject()) {
        (void)signal_val.As<v8::Object>()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "reason")).ToLocal(&reason);
    }
    if (reason.IsEmpty() || reason->IsUndefined()) {
        v8::Local<v8::Object> err = v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "The operation was aborted")).As<v8::Object>();
        (void)err->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "name"), v8::String::NewFromUtf8Literal(p_isolate, "AbortError"));
        reason = err;
    }

    (void)resolver->Reject(context, reason);
    onceCleanup(p_isolate, context, data);
}

static void onceResolveWrapper(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> data = args.Data().As<v8::Object>();
    
    v8::Local<v8::Value> resolver_val;
    if (!data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resolver")).ToLocal(&resolver_val)) return;
    v8::Local<v8::Promise::Resolver> resolver = resolver_val.As<v8::Promise::Resolver>();

    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, args.Length());
    for (int32_t i = 0; i < args.Length(); i++) (void)arr->Set(context, i, args[i]);
    
    (void)resolver->Resolve(context, arr);
    onceCleanup(p_isolate, context, data);
}

static void onceRejectWrapper(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> data = args.Data().As<v8::Object>();
    
    v8::Local<v8::Value> resolver_val;
    if (!data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resolver")).ToLocal(&resolver_val)) return;
    v8::Local<v8::Promise::Resolver> resolver = resolver_val.As<v8::Promise::Resolver>();

    v8::Local<v8::Value> error = args.Length() > 0 ? args[0] : v8::Undefined(p_isolate).As<v8::Value>();
    (void)resolver->Reject(context, error);
    onceCleanup(p_isolate, context, data);
}

void Events::once(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsObject()) return;

    v8::Local<v8::Object> emitter = args[0].As<v8::Object>();
    v8::Local<v8::Value> name = args[1];

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    v8::Local<v8::Object> data = v8::Object::New(p_isolate);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "resolver"), resolver);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "emitter"), emitter);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "name"), name);

    // Options support
    if (args.Length() >= 3 && args[2]->IsObject()) {
        v8::Local<v8::Object> options = args[2].As<v8::Object>();
        v8::Local<v8::Value> signal_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "signal")).ToLocal(&signal_val) && signal_val->IsObject()) {
            v8::Local<v8::Object> signal = signal_val.As<v8::Object>();
            v8::Local<v8::Value> aborted;
            if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "aborted")).ToLocal(&aborted) && aborted->BooleanValue(p_isolate)) {
                v8::Local<v8::Value> reason;
                (void)signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "reason")).ToLocal(&reason);
                if (reason->IsUndefined()) {
                    v8::Local<v8::Object> err = v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "The operation was aborted")).As<v8::Object>();
                    (void)err->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "name"), v8::String::NewFromUtf8Literal(p_isolate, "AbortError"));
                    reason = err;
                }
                (void)resolver->Reject(context, reason);
                return;
            }
            v8::Local<v8::Function> abort_listener = v8::Function::New(context, onceAbortWrapper, data).ToLocalChecked();
            v8::Local<v8::Value> add_fn;
            if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
                v8::Local<v8::Object> o = v8::Object::New(p_isolate);
                (void)o->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::True(p_isolate));
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "abort"), abort_listener, o };
                (void)add_fn.As<v8::Function>()->Call(context, signal, 3, argv);
                (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "signal"), signal);
                (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "abortListener"), abort_listener);
            }
        }
    }

    // Special case: if we are waiting for 'error', it resolves instead of rejecting
    bool is_error_event = false;
    if (name->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, name);
        if (strcmp(*utf8, "error") == 0) is_error_event = true;
    }

    v8::Local<v8::Function> resolve_wrapper = v8::Function::New(context, onceResolveWrapper, data).ToLocalChecked();
    v8::Local<v8::Function> reject_wrapper = v8::Function::New(context, onceRejectWrapper, data).ToLocalChecked();

    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "resolveWrapper"), resolve_wrapper);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "errorWrapper"), reject_wrapper);

    v8::Local<v8::Value> on_fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "once")).ToLocal(&on_fn) && on_fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { name, resolve_wrapper };
        (void)on_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
        if (!is_error_event) {
            v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), reject_wrapper };
            (void)on_fn.As<v8::Function>()->Call(context, emitter, 2, error_argv);
        }
    } else if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&on_fn) && on_fn->IsFunction()) {
        v8::Local<v8::Object> o = v8::Object::New(p_isolate);
        (void)o->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::True(p_isolate));
        v8::Local<v8::Value> argv[] = { name, resolve_wrapper, o };
        (void)on_fn.As<v8::Function>()->Call(context, emitter, 3, argv);
    }
}

// Helper to create the { value, done } object
static v8::Local<v8::Object> createIterResult(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> value, bool done) {
    v8::Local<v8::Object> result = v8::Object::New(p_isolate);
    (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "value"), value);
    (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, done));
    return result;
}

static void iterNext(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self;
    if (args.Data()->IsObject()) self = args.Data().As<v8::Object>();
    else self = args.This();

    v8::Local<v8::Value> error, queue_val, resolvers_val, done_val;
    (void)self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_error")).ToLocal(&error);
    (void)self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_queue")).ToLocal(&queue_val);
    (void)self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers")).ToLocal(&resolvers_val);
    (void)self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_done")).ToLocal(&done_val);

    if (!error->IsUndefined()) {
        v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
        (void)resolver->Reject(context, error);
        args.GetReturnValue().Set(resolver->GetPromise());
        return;
    }

    v8::Local<v8::Array> queue = queue_val.As<v8::Array>();
    if (queue->Length() > 0) {
        v8::Local<v8::Value> value = queue->Get(context, 0).ToLocalChecked();
        // Shift queue
        v8::Local<v8::Function> shift_fn = queue->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "shift")).ToLocalChecked().As<v8::Function>();
        (void)shift_fn->Call(context, queue, 0, nullptr);

        v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
        (void)resolver->Resolve(context, createIterResult(p_isolate, context, value, false));
        args.GetReturnValue().Set(resolver->GetPromise());
        return;
    }

    if (done_val->BooleanValue(p_isolate)) {
        v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
        (void)resolver->Resolve(context, createIterResult(p_isolate, context, v8::Undefined(p_isolate), true));
        args.GetReturnValue().Set(resolver->GetPromise());
        return;
    }

    // Pending resolver
    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    v8::Local<v8::Array> resolvers = resolvers_val.As<v8::Array>();
    (void)resolvers->Set(context, resolvers->Length(), resolver);
    args.GetReturnValue().Set(resolver->GetPromise());
}

static void iterReturn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self;
    if (args.Data()->IsObject()) self = args.Data().As<v8::Object>();
    else self = args.This();

    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_done"), v8::True(p_isolate));

    v8::Local<v8::Value> emitter_val, name_val, listener_val, error_listener_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_emitter")).ToLocal(&emitter_val) && emitter_val->IsObject()) {
        v8::Local<v8::Object> emitter = emitter_val.As<v8::Object>();
        v8::Local<v8::Value> remove_fn;
        if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_name")).ToLocal(&name_val) &&
                self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listener")).ToLocal(&listener_val)) {
                v8::Local<v8::Value> argv[] = { name_val, listener_val };
                (void)remove_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
            }
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_error_listener")).ToLocal(&error_listener_val)) {
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_listener_val };
                (void)remove_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
            }
        }
    }

    // Flush resolvers
    v8::Local<v8::Value> resolvers_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers")).ToLocal(&resolvers_val) && resolvers_val->IsArray()) {
        v8::Local<v8::Array> resolvers = resolvers_val.As<v8::Array>();
        for (uint32_t i = 0; i < resolvers->Length(); i++) {
            v8::Local<v8::Promise::Resolver> res = resolvers->Get(context, i).ToLocalChecked().As<v8::Promise::Resolver>();
            (void)res->Resolve(context, createIterResult(p_isolate, context, v8::Undefined(p_isolate), true));
        }
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers"), v8::Array::New(p_isolate, 0));
    }

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    (void)resolver->Resolve(context, createIterResult(p_isolate, context, v8::Undefined(p_isolate), true));
    args.GetReturnValue().Set(resolver->GetPromise());
}

static void onListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.Data().As<v8::Object>();

    v8::Local<v8::Value> done_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_done")).ToLocal(&done_val) && done_val->BooleanValue(p_isolate)) return;

    // Pack args
    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, args.Length());
    for (int32_t i = 0; i < args.Length(); i++) (void)arr->Set(context, i, args[i]);

    v8::Local<v8::Value> resolvers_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers")).ToLocal(&resolvers_val) && resolvers_val->IsArray()) {
        v8::Local<v8::Array> resolvers = resolvers_val.As<v8::Array>();
        if (resolvers->Length() > 0) {
            v8::Local<v8::Promise::Resolver> res = resolvers->Get(context, 0).ToLocalChecked().As<v8::Promise::Resolver>();
            // Shift resolvers
            v8::Local<v8::Function> shift_fn = resolvers->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "shift")).ToLocalChecked().As<v8::Function>();
            (void)shift_fn->Call(context, resolvers, 0, nullptr);

            (void)res->Resolve(context, createIterResult(p_isolate, context, arr, false));
            return;
        }
    }

    v8::Local<v8::Value> queue_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_queue")).ToLocal(&queue_val) && queue_val->IsArray()) {
        v8::Local<v8::Array> queue = queue_val.As<v8::Array>();
        (void)queue->Set(context, queue->Length(), arr);
    }
}

static void onErrorListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.Data().As<v8::Object>();

    v8::Local<v8::Value> error = args.Length() > 0 ? args[0] : v8::Undefined(p_isolate).As<v8::Value>();
    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_error"), error);

    // Flush resolvers with rejection
    v8::Local<v8::Value> resolvers_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers")).ToLocal(&resolvers_val) && resolvers_val->IsArray()) {
        v8::Local<v8::Array> resolvers = resolvers_val.As<v8::Array>();
        for (uint32_t i = 0; i < resolvers->Length(); i++) {
            v8::Local<v8::Promise::Resolver> res = resolvers->Get(context, i).ToLocalChecked().As<v8::Promise::Resolver>();
            (void)res->Reject(context, error);
        }
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers"), v8::Array::New(p_isolate, 0));
    }

    // Auto cleanup
    v8::Local<v8::Function> return_fn = v8::Function::New(context, iterReturn, self).ToLocalChecked();
    (void)return_fn->Call(context, self, 0, nullptr);
}

void Events::on(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsObject()) return;

    v8::Local<v8::Object> emitter = args[0].As<v8::Object>();
    v8::Local<v8::Value> name = args[1];

    v8::Local<v8::Object> iterator = v8::Object::New(p_isolate);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_queue"), v8::Array::New(p_isolate, 0));
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolvers"), v8::Array::New(p_isolate, 0));
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_error"), v8::Undefined(p_isolate));
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_done"), v8::False(p_isolate));
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_emitter"), emitter);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_name"), name);

    v8::Local<v8::Function> next_fn = v8::Function::New(context, iterNext, iterator).ToLocalChecked();
    v8::Local<v8::Function> return_fn = v8::Function::New(context, iterReturn, iterator).ToLocalChecked();
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "next"), next_fn);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "return"), return_fn);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "throw"), return_fn);

    (void)iterator->Set(context, v8::Symbol::GetAsyncIterator(p_isolate), v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        a.GetReturnValue().Set(a.This());
    }).ToLocalChecked());

    v8::Local<v8::Function> listener = v8::Function::New(context, onListener, iterator).ToLocalChecked();
    v8::Local<v8::Function> error_listener = v8::Function::New(context, onErrorListener, iterator).ToLocalChecked();
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_listener"), listener);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_error_listener"), error_listener);

    if (args.Length() >= 3 && args[2]->IsObject()) {
        v8::Local<v8::Object> options = args[2].As<v8::Object>();
        v8::Local<v8::Value> signal_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "signal")).ToLocal(&signal_val) && signal_val->IsObject()) {
            v8::Local<v8::Object> signal = signal_val.As<v8::Object>();
            v8::Local<v8::Value> aborted;
            if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "aborted")).ToLocal(&aborted) && aborted->BooleanValue(p_isolate)) {
                // Done immediately
                (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_done"), v8::True(p_isolate));
                v8::Local<v8::Value> reason;
                (void)signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "reason")).ToLocal(&reason);
                if (!reason->IsUndefined()) (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_error"), reason);
                args.GetReturnValue().Set(iterator);
                return;
            }
            v8::Local<v8::Value> add_fn;
            if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
                v8::Local<v8::Object> o = v8::Object::New(p_isolate);
                (void)o->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::True(p_isolate));
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "abort"), return_fn, o };
                (void)add_fn.As<v8::Function>()->Call(context, signal, 3, argv);
            }
        }
        v8::Local<v8::Value> close_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "close")).ToLocal(&close_val) && close_val->IsArray()) {
            v8::Local<v8::Array> close_arr = close_val.As<v8::Array>();
            v8::Local<v8::Value> add_fn;
            for (uint32_t i = 0; i < close_arr->Length(); i++) {
                v8::Local<v8::Value> close_name = close_arr->Get(context, i).ToLocalChecked();
                if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "once")).ToLocal(&add_fn) && add_fn->IsFunction()) {
                    v8::Local<v8::Value> argv[] = { close_name, return_fn };
                    (void)add_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
                }
            }
        }
    }

    v8::Local<v8::Value> on_fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { name, listener };
        (void)on_fn.As<v8::Function>()->Call(context, emitter, 2, argv);

        bool is_error_event = false;
        if (name->IsString()) {
            v8::String::Utf8Value utf8(p_isolate, name);
            if (strcmp(*utf8, "error") == 0) is_error_event = true;
        }
        if (!is_error_event) {
            v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_listener };
            (void)on_fn.As<v8::Function>()->Call(context, emitter, 2, error_argv);
        }
    } else if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&on_fn) && on_fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { name, listener };
        (void)on_fn.As<v8::Function>()->Call(context, emitter, 2, argv);
    }

    args.GetReturnValue().Set(iterator);
}

void Events::listenerCount(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 2) { args.GetReturnValue().Set(0); return; }
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> emitter = args[0]->ToObject(context).ToLocalChecked();
    v8::Local<v8::Value> count_fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listenerCount")).ToLocal(&count_fn) && count_fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { args[1] };
        v8::Local<v8::Value> result;
        if (count_fn.As<v8::Function>()->Call(context, emitter, 1, argv).ToLocal(&result)) {
            args.GetReturnValue().Set(result);
        }
    } else {
        args.GetReturnValue().Set(0);
    }
}

void Events::getEventListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 2) return;
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> emitter = args[0]->ToObject(context).ToLocalChecked();

    // 0) Check if it's a native EventTarget (non-Node) or if it has the spec-compliant behavior
    // Node v24: getEventListeners should work on any EventTarget.
    
    // 1) EventEmitter / NodeEventTarget with rawListeners()
    v8::Local<v8::Value> fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "rawListeners")).ToLocal(&fn) && fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { args[1] };
        v8::Local<v8::Value> result;
        if (fn.As<v8::Function>()->Call(context, emitter, 1, argv).ToLocal(&result)) {
            args.GetReturnValue().Set(result);
            return;
        }
    }

    // 2) Fallback to listeners() if available
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listeners")).ToLocal(&fn) && fn->IsFunction()) {
        v8::Local<v8::Value> argv[] = { args[1] };
        v8::Local<v8::Value> result;
        if (fn.As<v8::Function>()->Call(context, emitter, 1, argv).ToLocal(&result)) {
            args.GetReturnValue().Set(result);
            return;
        }
    }

    // 3) EventTarget / NodeEventTarget: read from internal _listeners map if present
    v8::Local<v8::Value> listeners_val;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listeners")).ToLocal(&listeners_val) && listeners_val->IsObject()) {
        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        v8::Local<v8::Value> arr_val;
        if (listeners->Get(context, args[1]).ToLocal(&arr_val) && arr_val->IsArray()) {
            args.GetReturnValue().Set(arr_val);
            return;
        }
    }

    // 4) Return empty array instead of undefined if nothing found (Node v24 behavior)
    args.GetReturnValue().Set(v8::Array::New(p_isolate, 0));
}

void Events::setMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // events.setMaxListeners(n[, ...eventTargets])
    if (args.Length() < 1) return;
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Value> n_val = args[0];

    if (args.Length() == 1) {
        if (!n_val->IsNumber()) {
            p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"n\" argument must be of type number")));
            return;
        }
        int32_t n = n_val->Int32Value(context).FromMaybe(-1);
        if (n < 0) {
            p_isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(p_isolate, "The value of \"n\" is out of range. It must be >= 0.")));
            return;
        }
        m_default_max_listeners = n;
        return;
    }

    for (int32_t i = 1; i < args.Length(); ++i) {
        if (!args[i]->IsObject()) continue;
        v8::Local<v8::Object> target = args[i]->ToObject(context).ToLocalChecked();
        v8::Local<v8::Value> set_fn;
        if (target->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "setMaxListeners"))
                .ToLocal(&set_fn) && set_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { n_val };
            (void)set_fn.As<v8::Function>()->Call(context, target, 1, argv);
        }
    }
}

void Events::getMaxListeners(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // events.getMaxListeners(emitterOrTarget)
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(m_default_max_listeners);
        return;
    }
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> emitter = args[0]->ToObject(context).ToLocalChecked();
    v8::Local<v8::Value> get_fn;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "getMaxListeners"))
            .ToLocal(&get_fn) && get_fn->IsFunction()) {
        v8::Local<v8::Value> result;
        if (get_fn.As<v8::Function>()->Call(context, emitter, 0, nullptr).ToLocal(&result)) {
            args.GetReturnValue().Set(result);
            return;
        }
    }

    // Node v24: For EventTarget that doesn't have getMaxListeners, return Infinity
    // (In our case, we return default if it's an EventEmitter-like, but for pure EventTarget it should be Infinity)
    // We check for _listeners to identify our EventTarget implementation
    v8::Local<v8::Value> listeners_val;
    if (emitter->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listeners")).ToLocal(&listeners_val) && listeners_val->IsObject()) {
        args.GetReturnValue().Set(v8::Number::New(p_isolate, std::numeric_limits<double>::infinity()));
        return;
    }

    args.GetReturnValue().Set(m_default_max_listeners);
}

void Events::addAbortListener(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsObject() || !args[1]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "The \"signal\" argument must be an object and \"listener\" must be a function")));
        return;
    }

    v8::Local<v8::Object> signal = args[0].As<v8::Object>();
    v8::Local<v8::Function> listener = args[1].As<v8::Function>();

    // Prepare Disposable
    v8::Local<v8::Object> disposable = v8::Object::New(p_isolate);
    v8::Local<v8::Object> data = v8::Object::New(p_isolate);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "signal"), signal);
    (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "listener"), listener);

    v8::Local<v8::Function> dispose_fn = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> data = args.Data().As<v8::Object>();
        v8::Local<v8::Value> signal_val, listener_val;

        if (data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "signal")).ToLocal(&signal_val) && signal_val->IsObject()) {
            v8::Local<v8::Object> signal = signal_val.As<v8::Object>();
            (void)data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "listener")).ToLocal(&listener_val);
            v8::Local<v8::Value> remove_fn;
            if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "removeEventListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "abort"), listener_val };
                (void)remove_fn.As<v8::Function>()->Call(context, signal, 2, argv);
            }
        }
    }, data).ToLocalChecked();

    (void)disposable->Set(context, v8::Symbol::GetDispose(p_isolate), dispose_fn);

    v8::Local<v8::Value> aborted;
    if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "aborted")).ToLocal(&aborted) && aborted->BooleanValue(p_isolate)) {
        (void)listener->Call(context, v8::Undefined(p_isolate), 0, nullptr);
    } else {
        v8::Local<v8::Value> add_fn;
        if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
            v8::Local<v8::Object> options = v8::Object::New(p_isolate);
            (void)options->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::True(p_isolate));
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "abort"), listener, options };
            (void)add_fn.As<v8::Function>()->Call(context, signal, 3, argv);
        }
    }

    args.GetReturnValue().Set(disposable);
}

void Events::bubbles(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(false);
        return;
    }
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> event = args[0].As<v8::Object>();
    v8::Local<v8::Value> bubbles_val;
    if (event->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "bubbles")).ToLocal(&bubbles_val)) {
        args.GetReturnValue().Set(bubbles_val->BooleanValue(p_isolate));
    } else {
        args.GetReturnValue().Set(false);
    }
}

void Events::stopPropagation(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) return;
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> event = args[0].As<v8::Object>();
    
    v8::Local<v8::Value> stop_fn;
    if (event->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "stopPropagation")).ToLocal(&stop_fn) && stop_fn->IsFunction()) {
        (void)stop_fn.As<v8::Function>()->Call(context, event, 0, nullptr);
    }
}

v8::Local<v8::FunctionTemplate> Events::createEventTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (!args.IsConstructCall()) return;
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        if (args.Length() < 1) return;
        v8::Local<v8::Object> self = args.This();
        
        bool bubbles = false;
        bool cancelable = false;
        if (args.Length() > 1 && args[1]->IsObject()) {
            v8::Local<v8::Object> options = args[1].As<v8::Object>();
            v8::Local<v8::Value> b_val, c_val;
            if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "bubbles")).ToLocal(&b_val)) bubbles = b_val->BooleanValue(p_isolate);
            if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "cancelable")).ToLocal(&c_val)) cancelable = c_val->BooleanValue(p_isolate);
        }

        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "type"), args[0]);
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "bubbles"), v8::Boolean::New(p_isolate, bubbles));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "cancelable"), v8::Boolean::New(p_isolate, cancelable));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "defaultPrevented"), v8::False(p_isolate));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "timeStamp"), v8::Number::New(p_isolate, 0)); // Should be real timestamp
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_stopped"), v8::False(p_isolate));
    });
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Event"));
    
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "preventDefault"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        v8::Local<v8::Value> cancelable;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "cancelable")).ToLocal(&cancelable) && cancelable->BooleanValue(p_isolate)) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "defaultPrevented"), v8::True(p_isolate));
        }
    }));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "stopPropagation"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        (void)args.This()->Set(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "_stopped"), v8::True(args.GetIsolate()));
    }));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "stopImmediatePropagation"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        (void)args.This()->Set(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "_stopped"), v8::True(args.GetIsolate()));
    }));

    return tmpl;
}

v8::Local<v8::FunctionTemplate> Events::createCustomEventTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> event_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (!args.IsConstructCall()) return;
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        // Call parent constructor logic (simulated since we are in C++)
        v8::Local<v8::Value> type = args.Length() > 0 ? args[0] : v8::Undefined(p_isolate).As<v8::Value>();
        v8::Local<v8::Value> options = args.Length() > 1 ? args[1] : v8::Object::New(p_isolate).As<v8::Value>();
        
        // Re-run Event logic
        bool bubbles = false;
        bool cancelable = false;
        v8::Local<v8::Value> detail = v8::Undefined(p_isolate);
        
        if (options->IsObject()) {
            v8::Local<v8::Object> opts = options.As<v8::Object>();
            v8::Local<v8::Value> b_val, c_val, d_val;
            if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "bubbles")).ToLocal(&b_val)) bubbles = b_val->BooleanValue(p_isolate);
            if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "cancelable")).ToLocal(&c_val)) cancelable = c_val->BooleanValue(p_isolate);
            if (opts->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "detail")).ToLocal(&d_val)) detail = d_val;
        }

        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "type"), type);
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "bubbles"), v8::Boolean::New(p_isolate, bubbles));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "cancelable"), v8::Boolean::New(p_isolate, cancelable));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "detail"), detail);
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "defaultPrevented"), v8::False(p_isolate));
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_stopped"), v8::False(p_isolate));
    });
    
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "CustomEvent"));
    tmpl->Inherit(event_tmpl);
    
    return tmpl;
}

v8::Local<v8::FunctionTemplate> Events::createEventTargetTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (!args.IsConstructCall()) return;
        args.This()->Set(args.GetIsolate()->GetCurrentContext(), 
            v8::String::NewFromUtf8Literal(args.GetIsolate(), "_listeners"), 
            v8::Object::New(args.GetIsolate())).Check();
    });
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "EventTarget"));
    
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "addEventListener"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listeners")).ToLocal(&listeners_val)) return;
        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        
        v8::Local<v8::Value> arr_val;
        if (!listeners->Get(context, args[0]).ToLocal(&arr_val) || !arr_val->IsArray()) {
            arr_val = v8::Array::New(p_isolate, 0);
            (void)listeners->Set(context, args[0], arr_val);
        }
        v8::Local<v8::Array> arr = arr_val.As<v8::Array>();
        (void)arr->Set(context, arr->Length(), args[1]);
    }));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeEventListener"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listeners")).ToLocal(&listeners_val)) return;
        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        
        v8::Local<v8::Value> arr_val;
        if (listeners->Get(context, args[0]).ToLocal(&arr_val) && arr_val->IsArray()) {
            v8::Local<v8::Array> arr = arr_val.As<v8::Array>();
            for (uint32_t i = 0; i < arr->Length(); i++) {
                if (arr->Get(context, i).ToLocalChecked()->StrictEquals(args[1])) {
                    // Primitive removal
                    v8::Local<v8::Array> new_arr = v8::Array::New(p_isolate, arr->Length() - 1);
                    for (uint32_t j = 0, k = 0; j < arr->Length(); j++) {
                        if (j == i) continue;
                        (void)new_arr->Set(context, k++, arr->Get(context, j).ToLocalChecked());
                    }
                    (void)listeners->Set(context, args[0], new_arr);
                    break;
                }
            }
        }
    }));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "dispatchEvent"), v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 1 || !args[0]->IsObject()) { args.GetReturnValue().Set(false); return; }
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        v8::Local<v8::Object> event = args[0].As<v8::Object>();
        v8::Local<v8::Value> type_val;
        if (!event->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "type")).ToLocal(&type_val)) { args.GetReturnValue().Set(false); return; }
        
        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_listeners")).ToLocal(&listeners_val)) { args.GetReturnValue().Set(true); return; }
        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        
        v8::Local<v8::Value> arr_val;
        if (listeners->Get(context, type_val).ToLocal(&arr_val) && arr_val->IsArray()) {
            v8::Local<v8::Array> arr = arr_val.As<v8::Array>();
            v8::Local<v8::Array> snapshot = v8::Array::New(p_isolate, arr->Length());
            for (uint32_t i = 0; i < arr->Length(); i++) snapshot->Set(context, i, arr->Get(context, i).ToLocalChecked()).Check();
            v8::Local<v8::Value> argv[] = { event };
            for (uint32_t i = 0; i < snapshot->Length(); i++) {
                // Check if propagation was stopped
                v8::Local<v8::Value> stopped;
                if (event->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_stopped")).ToLocal(&stopped) && stopped->BooleanValue(p_isolate)) {
                    break;
                }
                
                v8::Local<v8::Value> l = snapshot->Get(context, i).ToLocalChecked();
                if (l->IsFunction()) (void)l.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }
        args.GetReturnValue().Set(true);
    }));

    return tmpl;
}

v8::Local<v8::FunctionTemplate> Events::createNodeEventTargetTemplate(v8::Isolate* p_isolate) {
    // NodeEventTarget extends EventTarget and emulates a subset of EventEmitter API
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (!args.IsConstructCall()) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        // Initialise internal listener map (shared shape with EventTarget)
        (void)self->Set(context,
            v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners"),
            v8::Object::New(p_isolate_inner));
        // Optional per-instance max listeners for events.setMaxListeners
        (void)self->Set(context,
            v8::String::NewFromUtf8Literal(p_isolate_inner, "_maxListeners"),
            v8::Undefined(p_isolate_inner));
    });

    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "NodeEventTarget"));

    // Prototype: start from EventTarget behavior
    v8::Local<v8::FunctionTemplate> event_target_tmpl = createEventTargetTemplate(p_isolate);
    tmpl->Inherit(event_target_tmpl);

    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();

    // addListener(type, listener) → alias for addEventListener
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "addListener"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> add_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "addEventListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { args[0], args[1] };
            (void)add_fn.As<v8::Function>()->Call(context, self, 2, argv);
        }
        args.GetReturnValue().Set(self);
    }));

    // emit(type, arg) → dispatch arg to handlers for type
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "emit"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        if (args.Length() < 1) {
            args.GetReturnValue().Set(false);
            return;
        }

        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners")).ToLocal(&listeners_val) || !listeners_val->IsObject()) {
            args.GetReturnValue().Set(false);
            return;
        }

        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        v8::Local<v8::Value> arr_val;
        if (!listeners->Get(context, args[0]).ToLocal(&arr_val) || !arr_val->IsArray()) {
            args.GetReturnValue().Set(false);
            return;
        }

        v8::Local<v8::Array> arr = arr_val.As<v8::Array>();
        uint32_t len = arr->Length();
        if (len == 0) {
            args.GetReturnValue().Set(false);
            return;
        }

        // Snapshot to be robust against mutations while emitting
        v8::Local<v8::Array> snapshot = v8::Array::New(p_isolate_inner, len);
        for (uint32_t i = 0; i < len; i++) {
            snapshot->Set(context, i, arr->Get(context, i).ToLocalChecked()).Check();
        }

        v8::Local<v8::Value> arg = args.Length() > 1 ? args[1] : v8::Undefined(p_isolate_inner).As<v8::Value>();
        v8::Local<v8::Value> argv[] = { arg };

        for (uint32_t i = 0; i < snapshot->Length(); i++) {
            v8::Local<v8::Value> l = snapshot->Get(context, i).ToLocalChecked();
            if (l->IsFunction()) {
                (void)l.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }

        args.GetReturnValue().Set(true);
    }));

    // on(type, listener) → alias for addListener
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "on"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> add_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "addListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { args[0], args[1] };
            (void)add_fn.As<v8::Function>()->Call(context, self, 2, argv);
        }
        args.GetReturnValue().Set(self);
    }));

    // once(type, listener) → addEventListener with { once: true }
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "once"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> add_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "addEventListener")).ToLocal(&add_fn) && add_fn->IsFunction()) {
            v8::Local<v8::Object> options = v8::Object::New(p_isolate_inner);
            (void)options->Set(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "once"), v8::True(p_isolate_inner));
            v8::Local<v8::Value> argv[] = { args[0], args[1], options };
            (void)add_fn.As<v8::Function>()->Call(context, self, 3, argv);
        }
        args.GetReturnValue().Set(self);
    }));

    // removeListener(type, listener) → alias for removeEventListener
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeListener"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> remove_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "removeEventListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { args[0], args[1] };
            (void)remove_fn.As<v8::Function>()->Call(context, self, 2, argv);
        }
        args.GetReturnValue().Set(self);
    }));

    // off(type, listener[, options]) → alias for removeListener (ignore options)
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "off"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (args.Length() < 2) return;
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> remove_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "removeListener")).ToLocal(&remove_fn) && remove_fn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { args[0], args[1] };
            (void)remove_fn.As<v8::Function>()->Call(context, self, 2, argv);
        }
        args.GetReturnValue().Set(self);
    }));

    // removeAllListeners([type])
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeAllListeners"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners")).ToLocal(&listeners_val) || !listeners_val->IsObject()) {
            args.GetReturnValue().Set(self);
            return;
        }

        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        if (args.Length() > 0 && !args[0]->IsUndefined()) {
            (void)listeners->Delete(context, args[0]);
        } else {
            (void)self->Set(context,
                v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners"),
                v8::Object::New(p_isolate_inner));
        }
        args.GetReturnValue().Set(self);
    }));

    // eventNames() → keys of _listeners
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "eventNames"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners")).ToLocal(&listeners_val) || !listeners_val->IsObject()) {
            args.GetReturnValue().Set(v8::Array::New(p_isolate_inner, 0));
            return;
        }

        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        v8::Local<v8::Array> props;
        if (listeners->GetPropertyNames(context).ToLocal(&props)) {
            args.GetReturnValue().Set(props);
        } else {
            args.GetReturnValue().Set(v8::Array::New(p_isolate_inner, 0));
        }
    }));

    // listenerCount(type)
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "listenerCount"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        if (args.Length() < 1) {
            args.GetReturnValue().Set(0);
            return;
        }

        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners")).ToLocal(&listeners_val) || !listeners_val->IsObject()) {
            args.GetReturnValue().Set(0);
            return;
        }

        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        v8::Local<v8::Value> arr_val;
        if (!listeners->Get(context, args[0]).ToLocal(&arr_val) || !arr_val->IsArray()) {
            args.GetReturnValue().Set(0);
            return;
        }

        args.GetReturnValue().Set(static_cast<int32_t>(arr_val.As<v8::Array>()->Length()));
    }));

    // setMaxListeners(n)
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "setMaxListeners"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        if (args.Length() < 1 || !args[0]->IsNumber()) {
             p_isolate_inner->ThrowException(v8::Exception::TypeError(
                 v8::String::NewFromUtf8Literal(p_isolate_inner, "The \"n\" argument must be of type number")));
             return;
        }
        int32_t n = args[0]->Int32Value(context).FromMaybe(-1);
        if (n < 0) {
            p_isolate_inner->ThrowException(v8::Exception::RangeError(
                v8::String::NewFromUtf8Literal(p_isolate_inner, "The value of \"n\" is out of range. It must be >= 0.")));
            return;
        }
        (void)args.This()->Set(context,
            v8::String::NewFromUtf8Literal(p_isolate_inner, "_maxListeners"),
            args[0]);
        args.GetReturnValue().Set(args.This());
    }));

    // getMaxListeners()
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "getMaxListeners"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Value> val;
        if (args.This()->Get(context,
                v8::String::NewFromUtf8Literal(p_isolate_inner, "_maxListeners")).ToLocal(&val) && val->IsNumber()) {
            args.GetReturnValue().Set(val);
        } else {
            args.GetReturnValue().Set(m_default_max_listeners);
        }
    }));

    // rawListeners(type) so that events.getEventListeners can use it
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "rawListeners"),
               v8::FunctionTemplate::New(p_isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate_inner = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate_inner->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();

        v8::Local<v8::Value> listeners_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate_inner, "_listeners")).ToLocal(&listeners_val) || !listeners_val->IsObject()) {
            args.GetReturnValue().Set(v8::Array::New(p_isolate_inner, 0));
            return;
        }

        v8::Local<v8::Object> listeners = listeners_val.As<v8::Object>();
        v8::Local<v8::Value> arr_val;
        if (!listeners->Get(context, args[0]).ToLocal(&arr_val) || !arr_val->IsArray()) {
            args.GetReturnValue().Set(v8::Array::New(p_isolate_inner, 0));
            return;
        }

        args.GetReturnValue().Set(arr_val);
    }));

    return tmpl;
}
void Events::eeInit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() > 0 && args.This()->IsObject()) {
        eeConstructor(args);
    }
    args.GetReturnValue().Set(args.This());
}

void Events::staticGetDefaultMaxListeners(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(m_default_max_listeners);
}

void Events::staticSetDefaultMaxListeners(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    if (value->IsNumber()) {
        m_default_max_listeners = value->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(10);
    }
}

void Events::staticGetDefaultCaptureRejections(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(m_default_capture_rejections);
}

void Events::staticSetDefaultCaptureRejections(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    m_default_capture_rejections = value->BooleanValue(info.GetIsolate());
}

v8::Local<v8::FunctionTemplate> Events::getEventEmitterTemplate(v8::Isolate* p_isolate) {
    if (m_ee_tmpl.IsEmpty()) {
        m_ee_tmpl.Reset(p_isolate, createEventEmitterTemplate(p_isolate));
    }
    return m_ee_tmpl.Get(p_isolate);
}

} // namespace module
} // namespace zane