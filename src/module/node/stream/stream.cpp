#include "stream.h"
#include "task_queue.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
 
template<typename T>
static void StreamWeakCallback(const v8::WeakCallbackInfo<T>& data) {
    delete data.GetParameter();
}

namespace z8 {
namespace module {

v8::Persistent<v8::FunctionTemplate> Stream::m_readable_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_writable_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_duplex_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_transform_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_passthrough_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_web_readable_tmpl;
v8::Persistent<v8::FunctionTemplate> Stream::m_web_writable_tmpl;

v8::Local<v8::ObjectTemplate> Stream::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::FunctionTemplate> writable_tmpl = getWritableTemplate(p_isolate);
    v8::Local<v8::FunctionTemplate> duplex_tmpl = getDuplexTemplate(p_isolate);
    v8::Local<v8::FunctionTemplate> transform_tmpl = getTransformTemplate(p_isolate);
    v8::Local<v8::FunctionTemplate> passthrough_tmpl = getPassThroughTemplate(p_isolate);

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Readable"), readable_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Writable"), writable_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Duplex"), duplex_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "Transform"), transform_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "PassThrough"), passthrough_tmpl);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "pipeline"), v8::FunctionTemplate::New(p_isolate, pipeline));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "finished"), v8::FunctionTemplate::New(p_isolate, finished));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "compose"), v8::FunctionTemplate::New(p_isolate, compose));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "duplexPair"), v8::FunctionTemplate::New(p_isolate, duplexPair));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "addAbortSignal"), v8::FunctionTemplate::New(p_isolate, addAbortSignal));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getDefaultHighWaterMark"), v8::FunctionTemplate::New(p_isolate, getDefaultHighWaterMark));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "setDefaultHighWaterMark"), v8::FunctionTemplate::New(p_isolate, setDefaultHighWaterMark));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isErrored"), v8::FunctionTemplate::New(p_isolate, isErrored));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isReadable"), v8::FunctionTemplate::New(p_isolate, isReadable));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isWritable"), v8::FunctionTemplate::New(p_isolate, isWritable));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isDisturbed"), v8::FunctionTemplate::New(p_isolate, isDisturbed));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "destroy"), v8::FunctionTemplate::New(p_isolate, destroy));

    v8::Local<v8::ObjectTemplate> promises_tmpl = v8::ObjectTemplate::New(p_isolate);
    promises_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "pipeline"), v8::FunctionTemplate::New(p_isolate, pipelinePromise));
    promises_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "finished"), v8::FunctionTemplate::New(p_isolate, finishedPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "promises"), promises_tmpl);

    return tmpl;
}

v8::Local<v8::FunctionTemplate> Stream::getReadableTemplate(v8::Isolate* p_isolate) {
    if (m_readable_tmpl.IsEmpty()) {
        v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
        m_readable_tmpl.Reset(p_isolate, createReadableTemplate(p_isolate, ee_tmpl));
    }
    return m_readable_tmpl.Get(p_isolate);
}

v8::Local<v8::FunctionTemplate> Stream::getWritableTemplate(v8::Isolate* p_isolate) {
    if (m_writable_tmpl.IsEmpty()) {
        v8::Local<v8::FunctionTemplate> ee_tmpl = Events::getEventEmitterTemplate(p_isolate);
        m_writable_tmpl.Reset(p_isolate, createWritableTemplate(p_isolate, ee_tmpl));
    }
    return m_writable_tmpl.Get(p_isolate);
}

v8::Local<v8::FunctionTemplate> Stream::getDuplexTemplate(v8::Isolate* p_isolate) {
    if (m_duplex_tmpl.IsEmpty()) {
        v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
        v8::Local<v8::FunctionTemplate> writable_tmpl = getWritableTemplate(p_isolate);
        m_duplex_tmpl.Reset(p_isolate, createDuplexTemplate(p_isolate, readable_tmpl, writable_tmpl));
    }
    return m_duplex_tmpl.Get(p_isolate);
}

v8::Local<v8::FunctionTemplate> Stream::getTransformTemplate(v8::Isolate* p_isolate) {
    if (m_transform_tmpl.IsEmpty()) {
        v8::Local<v8::FunctionTemplate> duplex_tmpl = getDuplexTemplate(p_isolate);
        m_transform_tmpl.Reset(p_isolate, createTransformTemplate(p_isolate, duplex_tmpl));
    }
    return m_transform_tmpl.Get(p_isolate);
}

// --- Readable ---

v8::Local<v8::FunctionTemplate> Stream::createReadableTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, readableConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Readable"));
    tmpl->Inherit(ee_tmpl);
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);

    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "from"), v8::FunctionTemplate::New(p_isolate, readableFrom));

    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "read"), v8::FunctionTemplate::New(p_isolate, readableRead));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "push"), v8::FunctionTemplate::New(p_isolate, readablePush));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "pause"), v8::FunctionTemplate::New(p_isolate, readablePause));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "resume"), v8::FunctionTemplate::New(p_isolate, readableResume));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "destroy"), v8::FunctionTemplate::New(p_isolate, readableDestroy));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "setEncoding"), v8::FunctionTemplate::New(p_isolate, readableSetEncoding));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "pipe"), v8::FunctionTemplate::New(p_isolate, streamPipe));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "unpipe"), v8::FunctionTemplate::New(p_isolate, streamUnpipe));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "isPaused"), v8::FunctionTemplate::New(p_isolate, readableIsPaused));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "unshift"), v8::FunctionTemplate::New(p_isolate, readableUnshift));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "wrap"), v8::FunctionTemplate::New(p_isolate, readableWrap));
    
    // Collection methods
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "map"), v8::FunctionTemplate::New(p_isolate, readableMap));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "filter"), v8::FunctionTemplate::New(p_isolate, readableFilter));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "forEach"), v8::FunctionTemplate::New(p_isolate, readableForEach));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toArray"), v8::FunctionTemplate::New(p_isolate, readableToArray));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "some"), v8::FunctionTemplate::New(p_isolate, readableSome));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "find"), v8::FunctionTemplate::New(p_isolate, readableFind));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "every"), v8::FunctionTemplate::New(p_isolate, readableEvery));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "flatMap"), v8::FunctionTemplate::New(p_isolate, readableFlatMap));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "drop"), v8::FunctionTemplate::New(p_isolate, readableDrop));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "take"), v8::FunctionTemplate::New(p_isolate, readableTake));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "reduce"), v8::FunctionTemplate::New(p_isolate, readableReduce));
    
    // Property accessors
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readable"), 
        v8::FunctionTemplate::New(p_isolate, getReadableProperty));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableFlowing"), 
        v8::FunctionTemplate::New(p_isolate, getReadableFlowing));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableHighWaterMark"), 
        v8::FunctionTemplate::New(p_isolate, getReadableHighWaterMark));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableLength"), 
        v8::FunctionTemplate::New(p_isolate, getReadableLength));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableEncoding"), 
        v8::FunctionTemplate::New(p_isolate, getReadableEncoding));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableEnded"), 
        v8::FunctionTemplate::New(p_isolate, getReadableEnded));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableObjectMode"), 
        v8::FunctionTemplate::New(p_isolate, getReadableObjectMode));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableAborted"), 
        v8::FunctionTemplate::New(p_isolate, getReadableAborted));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "readableDidRead"), 
        v8::FunctionTemplate::New(p_isolate, getReadableDidRead));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "closed"), 
        v8::FunctionTemplate::New(p_isolate, getReadableClosed));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "destroyed"), 
        v8::FunctionTemplate::New(p_isolate, getReadableDestroyed));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "errored"), 
        v8::FunctionTemplate::New(p_isolate, getReadableErrored));
    
    // Additional methods
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "compose"), v8::FunctionTemplate::New(p_isolate, readableCompose));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "iterator"), v8::FunctionTemplate::New(p_isolate, readableIterator));

    // Symbol.asyncIterator - makes readable usable with for-await-of
    proto->Set(v8::Symbol::GetAsyncIterator(p_isolate), v8::FunctionTemplate::New(p_isolate, readableAsyncIterator));

    // Symbol.asyncDispose - for "await using" support
    proto->Set(v8::Symbol::GetAsyncDispose(p_isolate), v8::FunctionTemplate::New(p_isolate, readableAsyncDispose));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toWeb"), v8::FunctionTemplate::New(p_isolate, readableToWeb));

    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "from"), v8::FunctionTemplate::New(p_isolate, readableFrom));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fromWeb"), v8::FunctionTemplate::New(p_isolate, readableFromWeb));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "isDisturbed"), v8::FunctionTemplate::New(p_isolate, readableIsDisturbedStatic));

    return tmpl;
}

void Stream::readableConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    // Initialize EventEmitter part
    Events::eeConstructor(args);

    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> internal_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_internal")).ToLocal(&internal_val) && internal_val->IsExternal()) {
            self->SetInternalField(0, internal_val);
            // Also handle highWaterMark and read() if provided in options
            v8::Local<v8::Value> hwm;
            if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "highWaterMark")).ToLocal(&hwm) && hwm->IsNumber()) {
                StreamInternal* p_internal = static_cast<StreamInternal*>(internal_val.As<v8::External>()->Value());
                p_internal->m_high_water_mark = hwm->Uint32Value(context).FromMaybe(16384);
            }
            v8::Local<v8::Value> read_fn;
            if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn) && read_fn->IsFunction()) {
                (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_read"), read_fn);
            }

        }
    }

    StreamInternal* p_internal = new StreamInternal();
    p_internal->m_is_readable = true;
    
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> hwm;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "highWaterMark")).ToLocal(&hwm) && hwm->IsNumber()) {
            p_internal->m_high_water_mark = hwm->Uint32Value(context).FromMaybe(16384);
        }
        v8::Local<v8::Value> read_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn) && read_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_read"), read_fn);
        }
    }

    self->SetInternalField(0, v8::External::New(p_isolate, p_internal));

    // Cleanup on GC
    v8::Global<v8::Object> global_self(p_isolate, self);
    global_self.SetWeak(p_internal, StreamWeakCallback<StreamInternal>, v8::WeakCallbackType::kParameter);
}

void Stream::readableRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());

    // Respect state (ignore if already reading or ended)
    if (p_internal->m_reading || p_internal->m_ended || p_internal->m_destroyed) {
        args.GetReturnValue().Set(v8::Undefined(p_isolate));
        return;
    }

    v8::Local<v8::Value> read_fn_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_read")).ToLocal(&read_fn_val) && read_fn_val->IsFunction()) {
        p_internal->m_reading = true; // Mark as reading to prevent recursive calls
        v8::Local<v8::Function> read_fn = read_fn_val.As<v8::Function>();
        v8::Local<v8::Value> argv[1];
        int32_t argc = 0;
        if (args.Length() > 0 && !args[0]->IsUndefined()) {
            argv[0] = args[0];
            argc = 1;
        }
        (void)read_fn->Call(context, self, argc, argv);
        p_internal->m_reading = false; // Done reading
    }
    args.GetReturnValue().Set(v8::Undefined(p_isolate));
}

void Stream::readablePush(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() == 0 || args[0]->IsNull()) {
        // EOF
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end") };
            (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
        }
        args.GetReturnValue().Set(false);
        return;
    }

    // Emit 'data'
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), args[0] };
        (void)emit_val.As<v8::Function>()->Call(context, self, 2, argv);
    }

    // Flow data if not paused
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    if (!p_internal->m_paused) {
        // No longer enqueuing an automatic read() task here to avoid infinite loops.
        // The flow is driven by events and explicit read() calls.
    }

    args.GetReturnValue().Set(true);
}

void Stream::readablePause(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_paused = true;
    p_internal->m_flowing = false;
    
    // Emit 'pause' event
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "pause") };
        (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
    }
    
    args.GetReturnValue().Set(self);
}

void Stream::readableResume(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_paused = false;
    p_internal->m_flowing = true;
    
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    // Trigger read() to start data flow
    v8::Local<v8::Value> read_fn_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn_val) && read_fn_val->IsFunction()) {
        (void)read_fn_val.As<v8::Function>()->Call(context, self, 0, nullptr);
    }
    
    // Emit 'resume' event
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "resume") };
        (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
    }

    args.GetReturnValue().Set(self);
}

void Stream::readableSetEncoding(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Simplified: we don't store encoding in StreamInternal yet
    args.GetReturnValue().Set(args.This());
}

// Data structure to hold V8 Global handles for Readable.from task
struct ReadableFromTaskData {
    v8::Global<v8::Object> m_readable_instance;
    std::vector<v8::Global<v8::Value>> m_data;
    int32_t m_index;

    ReadableFromTaskData(v8::Isolate* p_isolate, v8::Local<v8::Object> readable_instance, v8::Local<v8::Array> data_array)
        : m_readable_instance(p_isolate, readable_instance), m_index(0) {
        for (uint32_t i = 0; i < data_array->Length(); ++i) {
            m_data.emplace_back(p_isolate, data_array->Get(p_isolate->GetCurrentContext(), i).ToLocalChecked());
        }
    }

    ReadableFromTaskData(v8::Isolate* p_isolate, v8::Local<v8::Object> readable_instance, std::vector<v8::Global<v8::Value>>&& data)
        : m_readable_instance(p_isolate, readable_instance), m_data(std::move(data)), m_index(0) {}

    ~ReadableFromTaskData() {
        m_readable_instance.Reset();
        for (auto& val : m_data) {
            val.Reset();
        }
    }
};

// The runner function for the TaskQueue for Readable.from
static void ReadableFromTaskRunner(v8::Isolate* p_isolate, v8::Local<v8::Context> context, z8::Task* p_task) {
    v8::HandleScope handle_scope(p_isolate);
    v8::Context::Scope context_scope(context);

    ReadableFromTaskData* p_data = static_cast<ReadableFromTaskData*>(p_task->p_data);
    v8::Local<v8::Object> readable_instance = p_data->m_readable_instance.Get(p_isolate);

    if (p_data->m_index < p_data->m_data.size()) {
        v8::Local<v8::Value> chunk = p_data->m_data[p_data->m_index].Get(p_isolate);
        v8::Local<v8::Value> argv[] = { chunk };
        
        v8::Local<v8::Value> push_fn_val;
        if (readable_instance->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn_val) && push_fn_val->IsFunction()) {
            (void)push_fn_val.As<v8::Function>()->Call(context, readable_instance, 1, argv);
        }

        p_data->m_index++;

        // Re-enqueue the task to push the next chunk
        z8::Task* p_next_task = new z8::Task();
        p_next_task->p_data = p_data;
        p_next_task->m_runner = ReadableFromTaskRunner;
        p_next_task->m_is_promise = false;
        z8::TaskQueue::getInstance().enqueue(p_next_task);
    } else {
        // End of data, push null
        v8::Local<v8::Value> argv[] = { v8::Null(p_isolate) };
        v8::Local<v8::Value> push_fn_val;
        if (readable_instance->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn_val) && push_fn_val->IsFunction()) {
            (void)push_fn_val.As<v8::Function>()->Call(context, readable_instance, 1, argv);
        }
        delete p_data; // Clean up data when done
    }
}

void Stream::readableFrom(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Iterable required for Readable.from")));
        return;
    }
    
    v8::Local<v8::Value> iterable = args[0];
    v8::Local<v8::Value> options = args.Length() > 1 ? args[1] : v8::Object::New(p_isolate).As<v8::Value>();
    
    // Create Readable
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor = readable_tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Value> ctor_args[] = { options };
    v8::Local<v8::Object> readable = readable_ctor->NewInstance(context, 1, ctor_args).ToLocalChecked();
    
    // Simple implementation: if iterable, iterate and push chunks asynchronously via Task
    if (iterable->IsObject()) {
        v8::Local<v8::Object> obj = iterable.As<v8::Object>();
        v8::Local<v8::Value> it_sym = v8::Symbol::GetIterator(p_isolate);
        v8::Local<v8::Value> it_fn_val;
        if (obj->Get(context, it_sym).ToLocal(&it_fn_val) && it_fn_val->IsFunction()) {
            v8::Local<v8::Function> it_fn = it_fn_val.As<v8::Function>();
            v8::Local<v8::Value> iterator;
            if (it_fn->Call(context, obj, 0, nullptr).ToLocal(&iterator) && iterator->IsObject()) {
                v8::Local<v8::Object> it_obj = iterator.As<v8::Object>();
                v8::Local<v8::Value> next_fn_val;
                if (it_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "next")).ToLocal(&next_fn_val) && next_fn_val->IsFunction()) {
                    v8::Local<v8::Function> next_fn = next_fn_val.As<v8::Function>();
                    std::vector<v8::Global<v8::Value>> data;
                    while (true) {
                        v8::Local<v8::Value> next_res;
                        if (!next_fn->Call(context, it_obj, 0, nullptr).ToLocal(&next_res) || !next_res->IsObject()) break;
                        v8::Local<v8::Object> res_obj = next_res.As<v8::Object>();
                        v8::Local<v8::Value> done;
                        if (res_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "done")).ToLocal(&done) && done->BooleanValue(p_isolate)) break;
                        v8::Local<v8::Value> val;
                        if (res_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "value")).ToLocal(&val)) {
                            data.emplace_back(p_isolate, val);
                        }
                    }
                    
                    // Enqueue tasks to push data asynchronously
                    if (!data.empty() || true) { // Always push null even if empty
                        ReadableFromTaskData* p_data = new ReadableFromTaskData(p_isolate, readable, std::move(data));
                        z8::Task* p_task = new z8::Task();
                        p_task->p_data = p_data;
                        p_task->m_runner = ReadableFromTaskRunner;
                        p_task->m_is_promise = false;
                        z8::TaskQueue::getInstance().enqueue(p_task);
                    }
                }
            }
        }
    }
    
    args.GetReturnValue().Set(readable);
}

void Stream::streamUnpipe(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Simplified: unpipe logic would require tracking piped destinations
    args.GetReturnValue().Set(args.This());
}

void Stream::writableSetDefaultEncoding(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(args.This());
}

void Stream::readableDestroy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_destroyed = true;
    p_internal->m_closed = true;

    // If error arg provided, emit 'error'
    if (args.Length() > 0 && !args[0]->IsUndefined() && !args[0]->IsNull()) {
        p_internal->m_errored = true;
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), args[0] };
            (void)emit_val.As<v8::Function>()->Call(context, self, 2, argv);
        }
    }

    // Emit 'close'
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "close") };
        (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
    }
    args.GetReturnValue().Set(self);
}

// --- Writable ---

v8::Local<v8::FunctionTemplate> Stream::createWritableTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> ee_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, writableConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Writable"));
    tmpl->Inherit(ee_tmpl);
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);

    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, writableWrite));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, writableEnd));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "destroy"), v8::FunctionTemplate::New(p_isolate, writableDestroy));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "cork"), v8::FunctionTemplate::New(p_isolate, writableCork));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "uncork"), v8::FunctionTemplate::New(p_isolate, writableUncork));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "setDefaultEncoding"), v8::FunctionTemplate::New(p_isolate, writableSetDefaultEncoding));
    
    // Property accessors
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writable"), 
        v8::FunctionTemplate::New(p_isolate, getWritableProperty));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableHighWaterMark"), 
        v8::FunctionTemplate::New(p_isolate, getWritableHighWaterMark));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableLength"), 
        v8::FunctionTemplate::New(p_isolate, getWritableLength));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableObjectMode"), 
        v8::FunctionTemplate::New(p_isolate, getWritableObjectMode));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableCorked"), 
        v8::FunctionTemplate::New(p_isolate, getWritableCorked));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableEnded"), 
        v8::FunctionTemplate::New(p_isolate, getWritableEnded));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableFinished"), 
        v8::FunctionTemplate::New(p_isolate, getWritableFinished));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableNeedDrain"), 
        v8::FunctionTemplate::New(p_isolate, getWritableNeedDrain));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableAborted"), 
        v8::FunctionTemplate::New(p_isolate, getWritableAborted));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "closed"), 
        v8::FunctionTemplate::New(p_isolate, getWritableClosed));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "destroyed"), 
        v8::FunctionTemplate::New(p_isolate, getWritableDestroyed));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "errored"), 
        v8::FunctionTemplate::New(p_isolate, getWritableErrored));

    // Symbol.asyncDispose
    proto->Set(v8::Symbol::New(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "Symbol.asyncDispose")), v8::FunctionTemplate::New(p_isolate, writableAsyncDispose));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toWeb"), v8::FunctionTemplate::New(p_isolate, writableToWeb));

    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fromWeb"), v8::FunctionTemplate::New(p_isolate, writableFromWeb));

    return tmpl;
}

void Stream::writableConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();

    Events::eeConstructor(args);

    StreamInternal* p_internal = new StreamInternal();
    p_internal->m_is_writable = true;
    self->SetInternalField(0, v8::External::New(p_isolate, p_internal));

    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> write_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_fn) && write_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_write"), write_fn);
        }
    }
 
    v8::Global<v8::Object> global_self(p_isolate, self);
    global_self.SetWeak(p_internal, StreamWeakCallback<StreamInternal>, v8::WeakCallbackType::kParameter);
}

void Stream::writableWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    // If _write is defined, call it. Otherwise, this is a base class that does nothing.
    v8::Local<v8::Value> write_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_write")).ToLocal(&write_val) && write_val->IsFunction()) {
        // args[0] is chunk, args[1] is encoding, args[2] is callback
        v8::Local<v8::Value> chunk = args[0];
        v8::Local<v8::Value> encoding = args.Length() > 1 ? args[1] : v8::Undefined(p_isolate).As<v8::Value>();
        v8::Local<v8::Value> callback = args.Length() > 2 ? args[2] : v8::Undefined(p_isolate).As<v8::Value>();
        
        if (!callback->IsFunction()) {
            // Provide a dummy callback if none was passed
            callback = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {}).ToLocalChecked();
        }

        v8::Local<v8::Value> argv[] = { chunk, encoding, callback };
        (void)write_val.As<v8::Function>()->Call(context, self, 3, argv);
    } else {
        // Default: If no callback, just return true
        if (args.Length() > 2 && args[2]->IsFunction()) {
            v8::Local<v8::Function> cb = args[2].As<v8::Function>();
            (void)cb->Call(context, self, 0, nullptr);
        }
    }

    args.GetReturnValue().Set(true);
}

void Stream::writableEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_ended = true;

    if (args.Length() > 0 && !args[0]->IsUndefined() && !args[0]->IsFunction()) {
        v8::Local<v8::Value> write_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_val) && write_val->IsFunction()) {
             v8::Local<v8::Value> chunk = args[0];
             v8::Local<v8::Value> write_argv[] = { chunk };
             (void)write_val.As<v8::Function>()->Call(context, self, 1, write_argv);
        }
    }

    // Call _flush if it exists
    v8::Local<v8::Value> flush_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_flush")).ToLocal(&flush_val) && flush_val->IsFunction()) {
        v8::Local<v8::Function> flush_cb = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_isolate = cb_args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Object> self = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            
            // if err? emit error
            if (cb_args.Length() > 0 && !cb_args[0]->IsNull() && !cb_args[0]->IsUndefined()) {
                v8::Local<v8::Value> emit_val;
                if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Value> emit_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), cb_args[0] };
                    (void)emit_val.As<v8::Function>()->Call(context, self, 2, emit_argv);
                }
            }
            
            // if data, push it
            if (cb_args.Length() > 1 && !cb_args[1]->IsNull() && !cb_args[1]->IsUndefined()) {
                v8::Local<v8::Value> push_fn;
                if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                    v8::Local<v8::Value> push_argv[] = { cb_args[1] };
                    (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
                }
            }
            
            // push null to end readable
            v8::Local<v8::Value> push_fn;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                v8::Local<v8::Value> push_argv[] = { v8::Null(p_isolate) };
                (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
            }
            
            // Set finished flag and emit 'finish'
            p_internal->m_finished = true;
            v8::Local<v8::Value> emit_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "finish") };
                (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }, self).ToLocalChecked();
        
        v8::Local<v8::Value> flush_argv[] = { flush_cb };
        (void)flush_val.As<v8::Function>()->Call(context, self, 1, flush_argv);
    } else {
        p_internal->m_finished = true;
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "finish") };
            (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
        }
    }

    if (args.Length() > 0 && args[args.Length()-1]->IsFunction()) {
        v8::Local<v8::Function> cb = args[args.Length()-1].As<v8::Function>();
        (void)cb->Call(context, self, 0, nullptr);
    }

    args.GetReturnValue().Set(args.This());
}

void Stream::writableDestroy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_destroyed = true;
    p_internal->m_closed = true;

    // If error arg provided, emit 'error'
    if (args.Length() > 0 && !args[0]->IsUndefined() && !args[0]->IsNull()) {
        p_internal->m_errored = true;
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), args[0] };
            (void)emit_val.As<v8::Function>()->Call(context, self, 2, argv);
        }
    }

    // Emit 'close'
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "close") };
        (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
    }
    args.GetReturnValue().Set(self);
}

// --- Duplex ---

v8::Local<v8::FunctionTemplate> Stream::createDuplexTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> readable_tmpl, v8::Local<v8::FunctionTemplate> writable_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, duplexConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Duplex"));
    tmpl->Inherit(readable_tmpl); // Duplex inherits from Readable in Node
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    
    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "from"), v8::FunctionTemplate::New(p_isolate, duplexFrom));
    
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    // Mix in Writable methods
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, writableWrite));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "end"), v8::FunctionTemplate::New(p_isolate, writableEnd));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "cork"), v8::FunctionTemplate::New(p_isolate, writableCork));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "uncork"), v8::FunctionTemplate::New(p_isolate, writableUncork));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "setDefaultEncoding"), v8::FunctionTemplate::New(p_isolate, writableSetDefaultEncoding));
    
    // Mix in Writable property accessors
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writable"), 
        v8::FunctionTemplate::New(p_isolate, getWritableProperty));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableHighWaterMark"), 
        v8::FunctionTemplate::New(p_isolate, getWritableHighWaterMark));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableLength"), 
        v8::FunctionTemplate::New(p_isolate, getWritableLength));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableObjectMode"), 
        v8::FunctionTemplate::New(p_isolate, getWritableObjectMode));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableCorked"), 
        v8::FunctionTemplate::New(p_isolate, getWritableCorked));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableEnded"), 
        v8::FunctionTemplate::New(p_isolate, getWritableEnded));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableFinished"), 
        v8::FunctionTemplate::New(p_isolate, getWritableFinished));
    proto->SetAccessorProperty(v8::String::NewFromUtf8Literal(p_isolate, "writableNeedDrain"), 
        v8::FunctionTemplate::New(p_isolate, getWritableNeedDrain));

    // duplex.allowHalfOpen property
    proto->SetAccessorProperty(
        v8::String::NewFromUtf8Literal(p_isolate, "allowHalfOpen"),
        v8::FunctionTemplate::New(p_isolate, getDuplexAllowHalfOpen),
        v8::FunctionTemplate::New(p_isolate, setDuplexAllowHalfOpen));

    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "toWeb"), v8::FunctionTemplate::New(p_isolate, duplexToWeb));

    // Static methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fromWeb"), v8::FunctionTemplate::New(p_isolate, duplexFromWeb));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "from"), v8::FunctionTemplate::New(p_isolate, duplexFrom));
    
    return tmpl;
}

void Stream::duplexConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Duplex is both readable and writable
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
 
    Events::eeConstructor(args);
 
    StreamInternal* p_internal = new StreamInternal();
    p_internal->m_is_readable = true;
    p_internal->m_is_writable = true;
    
    // Handle options
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        
        v8::Local<v8::Value> allow_half_open;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "allowHalfOpen")).ToLocal(&allow_half_open)) {
            p_internal->m_allow_half_open = allow_half_open->BooleanValue(p_isolate);
        }
        
        v8::Local<v8::Value> hwm;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "highWaterMark")).ToLocal(&hwm) && hwm->IsNumber()) {
            p_internal->m_high_water_mark = hwm->Uint32Value(context).FromMaybe(16384);
        }

        v8::Local<v8::Value> read_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn) && read_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_read"), read_fn);
        }

        v8::Local<v8::Value> write_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_fn) && write_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_write"), write_fn);
        }
    }

    self->SetInternalField(0, v8::External::New(p_isolate, p_internal));
 
    v8::Global<v8::Object> global_self(p_isolate, self);
    global_self.SetWeak(p_internal, StreamWeakCallback<StreamInternal>, v8::WeakCallbackType::kParameter);
}

// --- Transform ---

v8::Local<v8::FunctionTemplate> Stream::createTransformTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> duplex_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, transformConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "Transform"));
    tmpl->Inherit(duplex_tmpl);
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "_write"), v8::FunctionTemplate::New(p_isolate, transformWrite));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "_transform"), v8::FunctionTemplate::New(p_isolate, transformTransform));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "_flush"), v8::FunctionTemplate::New(p_isolate, transformFlush));
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "destroy"), v8::FunctionTemplate::New(p_isolate, transformDestroy));
    
    return tmpl;
}

void Stream::transformConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    readableConstructor(args);
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_is_writable = true;

    // Handle options: transform, flush, etc.
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> transform_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "transform")).ToLocal(&transform_fn) && transform_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_transform"), transform_fn);
        }
        v8::Local<v8::Value> flush_fn;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "flush")).ToLocal(&flush_fn) && flush_fn->IsFunction()) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_flush"), flush_fn);
        }
    }
}

void Stream::transformWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Value> transform_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_transform")).ToLocal(&transform_val) && transform_val->IsFunction()) {
        
        // Create a callback that pushes data and then calls the original callback
        v8::Local<v8::Object> cb_data = v8::Object::New(p_isolate);
        cb_data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "self"), self).Check();
        if (args.Length() > 2 && args[2]->IsFunction()) {
            cb_data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "cb"), args[2]).Check();
        }
        
        v8::Local<v8::Function> transform_cb = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_isolate = cb_args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Object> cb_data = cb_args.Data().As<v8::Object>();
            v8::Local<v8::Object> self = cb_data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "self")).ToLocalChecked().As<v8::Object>();
            
            // if err? emit error
            if (cb_args.Length() > 0 && !cb_args[0]->IsNull() && !cb_args[0]->IsUndefined()) {
                v8::Local<v8::Value> emit_val;
                if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Value> emit_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), cb_args[0] };
                    (void)emit_val.As<v8::Function>()->Call(context, self, 2, emit_argv);
                }
            }
            
            // if data, push it
            if (cb_args.Length() > 1 && !cb_args[1]->IsNull() && !cb_args[1]->IsUndefined()) {
                v8::Local<v8::Value> push_fn;
                if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                    v8::Local<v8::Value> push_argv[] = { cb_args[1] };
                    (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
                }
            }
            
            // Call original callback
            v8::Local<v8::Value> orig_cb;
            if (cb_data->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "cb")).ToLocal(&orig_cb) && orig_cb->IsFunction()) {
                (void)orig_cb.As<v8::Function>()->Call(context, v8::Null(p_isolate), 0, nullptr);
            }
        }, cb_data).ToLocalChecked();
        
        v8::Local<v8::Value> argv[] = { args[0], args[1], transform_cb };
        (void)transform_val.As<v8::Function>()->Call(context, self, 3, argv);
    }
}

void Stream::transformTransform(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Default: push what you get (identity transform)
    readablePush(args);
    if (args.Length() > 2 && args[2]->IsFunction()) {
        v8::Local<v8::Function> cb = args[2].As<v8::Function>();
        (void)cb->Call(args.GetIsolate()->GetCurrentContext(), args.This(), 0, nullptr);
    }
}

void Stream::transformFlush(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() > 0 && args[0]->IsFunction()) {
        v8::Local<v8::Function> cb = args[0].As<v8::Function>();
        (void)cb->Call(args.GetIsolate()->GetCurrentContext(), args.This(), 0, nullptr);
    }
}

// --- Pipe ---

void Stream::streamPipe(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() < 1 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Dest must be a stream")));
        return;
    }
    
    v8::Local<v8::Object> dest = args[0].As<v8::Object>();
    
    // Very simplified pipe: listen for 'data' on source, call write() on dest
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        
        v8::Local<v8::Object> data = v8::Object::New(p_isolate);
        data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "dest"), dest).Check();
        
        v8::Local<v8::Function> data_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Local<v8::Object> dest_obj = args.Data().As<v8::Object>()->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "dest")).ToLocalChecked().As<v8::Object>();
            v8::Local<v8::Value> write_fn;
            if (dest_obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "write")).ToLocal(&write_fn) && write_fn->IsFunction()) {
                v8::Local<v8::Value> chunk = args[0];
                // Pass a dummy callback to write()
                v8::Local<v8::Value> cb = v8::Function::New(args.GetIsolate()->GetCurrentContext(), [](const v8::FunctionCallbackInfo<v8::Value>& args) {}).ToLocalChecked();
                v8::Local<v8::Value> write_argv[] = { chunk, v8::Undefined(args.GetIsolate()), cb };
                (void)write_fn.As<v8::Function>()->Call(args.GetIsolate()->GetCurrentContext(), dest_obj, 3, write_argv);
            }
        }, data).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), data_handler };
        (void)on_val.As<v8::Function>()->Call(context, self, 2, on_argv);
        
        // Handle 'end' -> dest.end()
        v8::Local<v8::Function> end_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Local<v8::Object> dest_obj = args.Data().As<v8::Object>()->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "dest")).ToLocalChecked().As<v8::Object>();
            v8::Local<v8::Value> end_fn;
            if (dest_obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "end")).ToLocal(&end_fn) && end_fn->IsFunction()) {
                (void)end_fn.As<v8::Function>()->Call(args.GetIsolate()->GetCurrentContext(), dest_obj, 0, nullptr);
            }
        }, data).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end"), end_handler };
        (void)on_val.As<v8::Function>()->Call(context, self, 2, end_argv);

        // Put the stream in flowing mode
        v8::Local<v8::Value> resume_fn_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resume")).ToLocal(&resume_fn_val) && resume_fn_val->IsFunction()) {
            (void)resume_fn_val.As<v8::Function>()->Call(context, self, 0, nullptr);
        }

        // Start flowing data by calling read() on the source stream
        v8::Local<v8::Value> read_fn_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn_val) && read_fn_val->IsFunction()) {
            (void)read_fn_val.As<v8::Function>()->Call(context, self, 0, nullptr);
        }
    }
    
    args.GetReturnValue().Set(dest);
}

void Stream::pipeline(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 2) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Not enough arguments to pipeline()")));
        return;
    }

    // Basic pipeline implementation: connect all streams in the arguments
    v8::Local<v8::Value> current = args[0];
    v8::Local<v8::Value> callback = v8::Null(p_isolate);

    int32_t stream_count = args.Length();
    if (args[args.Length() - 1]->IsFunction()) {
        callback = args[args.Length() - 1];
        stream_count--;
    }

    for (int32_t i = 0; i < stream_count - 1; ++i) {
        v8::Local<v8::Value> source = args[i];
        v8::Local<v8::Value> dest = args[i + 1];

        if (source->IsObject() && dest->IsObject()) {
            v8::Local<v8::Object> source_obj = source.As<v8::Object>();
            v8::Local<v8::Value> pipe_fn;
            if (source_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "pipe")).ToLocal(&pipe_fn) && pipe_fn->IsFunction()) {
                v8::Local<v8::Value> pipe_argv[] = { dest };
                (void)pipe_fn.As<v8::Function>()->Call(context, source_obj, 1, pipe_argv);
            }
        }
    }

    if (callback->IsFunction()) {
        // Wait for the last stream to finish/error before calling the callback
        v8::Local<v8::Value> last_stream = args[stream_count - 1];
        if (last_stream->IsObject()) {
            v8::Local<v8::Object> last_stream_obj = last_stream.As<v8::Object>();
            v8::Local<v8::Value> on_fn;
            if (last_stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
                v8::Local<v8::Value> finish_evt = v8::String::NewFromUtf8Literal(p_isolate, "finish");
                v8::Local<v8::Value> end_evt = v8::String::NewFromUtf8Literal(p_isolate, "end");
                v8::Local<v8::Value> close_evt = v8::String::NewFromUtf8Literal(p_isolate, "close");
                v8::Local<v8::Value> error_evt = v8::String::NewFromUtf8Literal(p_isolate, "error");

                auto final_cb = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                    v8::Isolate* p_isolate = args.GetIsolate();
                    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                    v8::Local<v8::Function> cb = args.Data().As<v8::Function>();
                    v8::Local<v8::Value> err = args.Length() > 0 ? args[0] : v8::Null(p_isolate).As<v8::Value>();
                    v8::Local<v8::Value> argv[] = { err };
                    (void)cb->Call(context, v8::Undefined(p_isolate), 1, argv);
                };

                v8::Local<v8::Function> cb_fn = v8::Local<v8::Function>::Cast(callback);
                v8::Local<v8::Function> wrapper = v8::Function::New(context, final_cb, cb_fn).ToLocalChecked();

                v8::Local<v8::Value> on_argv_finish[] = { finish_evt, wrapper };
                (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, on_argv_finish);
                
                v8::Local<v8::Value> on_argv_end[] = { end_evt, wrapper };
                (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, on_argv_end);

                v8::Local<v8::Value> on_argv_close[] = { close_evt, wrapper };
                (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, on_argv_close);

                v8::Local<v8::Value> on_argv_error[] = { error_evt, wrapper };
                (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, on_argv_error);
                
                // For Readable source, call resume()
                v8::Local<v8::Value> source = args[0];
                if (source->IsObject()) {
                    v8::Local<v8::Object> source_obj = source.As<v8::Object>();
                    v8::Local<v8::Value> resume_fn;
                    if (source_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resume")).ToLocal(&resume_fn) && resume_fn->IsFunction()) {
                        (void)resume_fn.As<v8::Function>()->Call(context, source_obj, 0, nullptr);
                    }
                }
            } else {
                v8::Local<v8::Value> callback_argv[] = { v8::Null(p_isolate) };
                (void)callback.As<v8::Function>()->Call(context, v8::Null(p_isolate), 1, callback_argv);
            }
        } else {
            v8::Local<v8::Value> callback_argv[] = { v8::Null(p_isolate) };
            (void)callback.As<v8::Function>()->Call(context, v8::Null(p_isolate), 1, callback_argv);
        }
    }

    args.GetReturnValue().Set(args[stream_count - 1]);
}

void Stream::finished(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "First argument must be a stream")));
        return;
    }

    v8::Local<v8::Object> stream_obj = args[0].As<v8::Object>();
    v8::Local<v8::Value> callback = args.Length() > 1 ? args[1] : v8::Null(p_isolate).As<v8::Value>();

    if (callback->IsFunction()) {
        // Simplified: listen for 'end' or 'finish' or 'close' or 'error'
        v8::Local<v8::Value> on_fn;
        if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
            v8::Local<v8::Value> events[] = {
                v8::String::NewFromUtf8Literal(p_isolate, "end"),
                v8::String::NewFromUtf8Literal(p_isolate, "finish"),
                v8::String::NewFromUtf8Literal(p_isolate, "close")
            };

            for (auto& evt : events) {
                v8::Local<v8::Value> on_argv[] = { evt, callback };
                (void)on_fn.As<v8::Function>()->Call(context, stream_obj, 2, on_argv);
            }

            // Handle error separately to pass the error object
            v8::Local<v8::Value> error_evt = v8::String::NewFromUtf8Literal(p_isolate, "error");
            v8::Local<v8::Value> error_argv[] = { error_evt, callback };
            (void)on_fn.As<v8::Function>()->Call(context, stream_obj, 2, error_argv);
        }
    }

    args.GetReturnValue().Set(v8::Undefined(p_isolate));
}

void Stream::pipelinePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() < 2) {
        resolver->Reject(context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Not enough arguments to pipeline()")));
        return;
    }

    // Connect streams using pipe()
    int32_t stream_count = args.Length();
    for (int32_t i = 0; i < stream_count - 1; ++i) {
        v8::Local<v8::Value> source = args[i];
        v8::Local<v8::Value> dest = args[i + 1];

        if (source->IsObject() && dest->IsObject()) {
            v8::Local<v8::Object> source_obj = source.As<v8::Object>();
            v8::Local<v8::Value> pipe_fn;
            if (source_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "pipe")).ToLocal(&pipe_fn) && pipe_fn->IsFunction()) {
                v8::Local<v8::Value> pipe_argv[] = { dest };
                (void)pipe_fn.As<v8::Function>()->Call(context, source_obj, 1, pipe_argv);
            }

            // Attach error listener to each stream to reject the promise
            v8::Local<v8::Value> on_fn;
            if (source_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
                v8::Local<v8::Function> error_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& error_args) {
                    v8::Local<v8::Promise::Resolver> local_resolver = error_args.Data().As<v8::Promise::Resolver>();
                    v8::Local<v8::Value> error = error_args.Length() > 0 ? error_args[0] : v8::Exception::Error(v8::String::NewFromUtf8Literal(error_args.GetIsolate(), "Pipeline stream error"));
                    (void)local_resolver->Reject(error_args.GetIsolate()->GetCurrentContext(), error);
                }, resolver).ToLocalChecked();

                v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_handler };
                (void)on_fn.As<v8::Function>()->Call(context, source_obj, 2, error_argv);
            }
        }
    }

    // Resolve the promise when the last stream finishes or closes
    v8::Local<v8::Value> last_stream_val = args[stream_count - 1];
    if (last_stream_val->IsObject()) {
        v8::Local<v8::Object> last_stream_obj = last_stream_val.As<v8::Object>();
        v8::Local<v8::Value> on_fn;
        if (last_stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
            
            // Success handler
            v8::Local<v8::Function> success_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& success_args) {
                v8::Local<v8::Promise::Resolver> local_resolver = success_args.Data().As<v8::Promise::Resolver>();
                (void)local_resolver->Resolve(success_args.GetIsolate()->GetCurrentContext(), v8::Undefined(success_args.GetIsolate()));
            }, resolver).ToLocalChecked();

            v8::Local<v8::Value> success_events[] = {
                v8::String::NewFromUtf8Literal(p_isolate, "finish"),
                v8::String::NewFromUtf8Literal(p_isolate, "close")
            };

            for (auto& evt : success_events) {
                v8::Local<v8::Value> on_argv[] = { evt, success_handler };
                (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, on_argv);
            }

            // Error handler for the last stream (in case it wasn't caught earlier)
            v8::Local<v8::Function> error_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& error_args) {
                v8::Local<v8::Promise::Resolver> local_resolver = error_args.Data().As<v8::Promise::Resolver>();
                v8::Local<v8::Value> error = error_args.Length() > 0 ? error_args[0] : v8::Exception::Error(v8::String::NewFromUtf8Literal(error_args.GetIsolate(), "Pipeline error on last stream"));
                (void)local_resolver->Reject(error_args.GetIsolate()->GetCurrentContext(), error);
            }, resolver).ToLocalChecked();

            v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_handler };
            (void)on_fn.As<v8::Function>()->Call(context, last_stream_obj, 2, error_argv);
        } else {
            // If the last stream doesn't have an 'on' method, resolve immediately
            (void)resolver->Resolve(context, v8::Undefined(p_isolate));
        }
    } else {
        // If the last argument is not an object, resolve immediately
        (void)resolver->Resolve(context, v8::Undefined(p_isolate));
    }
}

void Stream::finishedPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    if (args.Length() < 1 || !args[0]->IsObject()) {
        resolver->Reject(context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "First argument must be a stream")));
        return;
    }

    v8::Local<v8::Object> stream_obj = args[0].As<v8::Object>();
    
    // Check if the stream is already finished, ended, closed or destroyed
    if (stream_obj->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = stream_obj->GetInternalField(0).As<v8::External>();
        if (!ext.IsEmpty()) {
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            if (p_internal->m_finished || p_internal->m_closed || p_internal->m_destroyed || p_internal->m_ended || p_internal->m_errored) {
                if (p_internal->m_errored) {
                    resolver->Reject(context, v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Stream errored previously")));
                } else {
                    resolver->Resolve(context, v8::Undefined(p_isolate));
                }
                return;
            }
        }
    }

    v8::Local<v8::Value> on_fn;

    if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn) && on_fn->IsFunction()) {
        v8::Local<v8::Function> success_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Local<v8::Promise::Resolver> local_resolver = args.Data().As<v8::Promise::Resolver>();
            (void)local_resolver->Resolve(args.GetIsolate()->GetCurrentContext(), v8::Undefined(args.GetIsolate()));
        }, resolver).ToLocalChecked();

        v8::Local<v8::Value> success_events[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            v8::String::NewFromUtf8Literal(p_isolate, "finish"),
            v8::String::NewFromUtf8Literal(p_isolate, "close")
        };

        for (auto& evt : success_events) {
            v8::Local<v8::Value> on_argv[] = { evt, success_handler };
            (void)on_fn.As<v8::Function>()->Call(context, stream_obj, 2, on_argv);
        }

        v8::Local<v8::Function> error_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Local<v8::Promise::Resolver> local_resolver = args.Data().As<v8::Promise::Resolver>();
            v8::Local<v8::Value> error = args.Length() > 0 ? args[0] : v8::Exception::Error(v8::String::NewFromUtf8Literal(args.GetIsolate(), "Stream error"));
            (void)local_resolver->Reject(args.GetIsolate()->GetCurrentContext(), error);
        }, resolver).ToLocalChecked();

        v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_handler };
        (void)on_fn.As<v8::Function>()->Call(context, stream_obj, 2, error_argv);

        // Check if it's a readable stream and start flowing data
        v8::Local<v8::External> ext = stream_obj->GetInternalField(0).As<v8::External>();
        if (!ext.IsEmpty()) {
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            if (p_internal->m_is_readable) {
                v8::Local<v8::Value> read_fn_val;
                if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn_val) && read_fn_val->IsFunction()) {
                    (void)read_fn_val.As<v8::Function>()->Call(context, stream_obj, 0, nullptr);
                }
            }
        }
    } else {
        resolver->Resolve(context, v8::Undefined(p_isolate));
    }
}

// --- Readable Property Getters ---

void Stream::getReadableProperty(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_is_readable && !p_internal->m_destroyed && !p_internal->m_ended);
}

void Stream::getReadableFlowing(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    
    if (p_internal->m_flowing) {
        args.GetReturnValue().Set(true);
    } else if (p_internal->m_paused) {
        args.GetReturnValue().Set(false);
    } else {
        args.GetReturnValue().Set(v8::Null(p_isolate));
    }
}

void Stream::getReadableHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_high_water_mark);
}

void Stream::getReadableLength(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(static_cast<uint32_t>(p_internal->m_buffer.size()));
}

void Stream::getReadableEncoding(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::Null(p_isolate));
}

void Stream::getReadableEnded(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_ended);
}

void Stream::getReadableObjectMode(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(false);
}

// --- Writable Property Getters ---

void Stream::getWritableProperty(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_is_writable && !p_internal->m_destroyed && !p_internal->m_ended);
}

void Stream::getWritableHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_high_water_mark);
}

void Stream::getWritableLength(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(static_cast<uint32_t>(p_internal->m_buffer.size()));
}

void Stream::getWritableObjectMode(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(false);
}

void Stream::getWritableCorked(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_cork_count);
}

void Stream::getWritableEnded(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_ended);
}

void Stream::getWritableFinished(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_finished);
}

void Stream::getWritableNeedDrain(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_buffer.size() >= p_internal->m_high_water_mark);
}

// --- New Readable Methods ---

void Stream::readableIsPaused(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_paused);
}

void Stream::readableUnshift(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() == 0) {
        return;
    }
    
    v8::Local<v8::Value> chunk = args[0];
    
    // If null, signal EOF
    if (chunk->IsNull()) {
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end") };
            (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
        }
        return;
    }
    
    // Store the chunk internally (simplified - in real implementation would prepend to buffer)
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    
    // Emit 'readable' event to signal data is available
    v8::Local<v8::Value> emit_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
        v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "readable") };
        (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
    }
}

void Stream::readableWrap(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() < 1 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Stream argument required")));
        return;
    }
    
    v8::Local<v8::Object> old_stream = args[0].As<v8::Object>();
    
    // Listen to 'data' events from old stream and push to new stream
    v8::Local<v8::Value> on_val;
    if (old_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> data_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_isolate = cb_args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Object> self = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> push_fn;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                v8::Local<v8::Value> argv[] = { cb_args[0] };
                (void)push_fn.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }, self).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), data_handler };
        (void)on_val.As<v8::Function>()->Call(context, old_stream, 2, on_argv);
        
        // Listen to 'end' event
        v8::Local<v8::Function> end_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_isolate = cb_args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Object> self = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> push_fn;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                v8::Local<v8::Value> argv[] = { v8::Null(p_isolate) };
                (void)push_fn.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }, self).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end"), end_handler };
        (void)on_val.As<v8::Function>()->Call(context, old_stream, 2, end_argv);
    }
    
    args.GetReturnValue().Set(self);
}

// --- Cork/Uncork Implementation ---

void Stream::writableCork(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_cork_count++;
    p_internal->m_corked = true;
    args.GetReturnValue().Set(self);
}

void Stream::writableUncork(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    
    if (p_internal->m_cork_count > 0) {
        p_internal->m_cork_count--;
    }
    
    if (p_internal->m_cork_count == 0) {
        p_internal->m_corked = false;
        
        // Emit 'drain' if buffer was full
        if (p_internal->m_buffer.size() >= p_internal->m_high_water_mark) {
            v8::Local<v8::Value> emit_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "drain") };
                (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
            }
        }
    }
    
    args.GetReturnValue().Set(self);
}

// --- Stream Utility Functions ---

void Stream::compose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "At least 1 stream required for compose")));
        return;
    }
    
    // Connect all streams using pipe()
    for (int32_t i = 0; i < args.Length() - 1; ++i) {
        v8::Local<v8::Value> current = args[i];
        v8::Local<v8::Value> next = args[i + 1];
        
        if (current->IsObject() && next->IsObject()) {
            v8::Local<v8::Object> current_obj = current.As<v8::Object>();
            v8::Local<v8::Value> pipe_fn;
            if (current_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "pipe")).ToLocal(&pipe_fn) && pipe_fn->IsFunction()) {
                v8::Local<v8::Value> pipe_argv[] = { next };
                (void)pipe_fn.As<v8::Function>()->Call(context, current_obj, 1, pipe_argv);
            }
        }
    }
    
    // Create a Duplex stream that acts as a proxy for the chain
    v8::Local<v8::FunctionTemplate> duplex_tmpl = getDuplexTemplate(p_isolate);
    v8::Local<v8::Function> duplex_ctor = duplex_tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Object> duplex_instance = duplex_ctor->NewInstance(context, 0, nullptr).ToLocalChecked();
    
    v8::Local<v8::Object> first_stream = args[0].As<v8::Object>();
    v8::Local<v8::Object> last_stream = args[args.Length() - 1].As<v8::Object>();
    
    // Link first and last streams to the duplex instance for internal reference
    (void)duplex_instance->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_composeFirst"), first_stream);
    (void)duplex_instance->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_composeLast"), last_stream);
    
    // Proxy write() to the first stream
    auto proxy_write = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        v8::Local<v8::Value> first_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_composeFirst")).ToLocal(&first_val) && first_val->IsObject()) {
            v8::Local<v8::Object> first = first_val.As<v8::Object>();
            v8::Local<v8::Value> write_fn;
            if (first->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_fn) && write_fn->IsFunction()) {
                std::vector<v8::Local<v8::Value>> argv_vec;
                for (int32_t i = 0; i < args.Length(); ++i) argv_vec.push_back(args[i]);
                args.GetReturnValue().Set(write_fn.As<v8::Function>()->Call(context, first, argv_vec.size(), argv_vec.data()).ToLocalChecked());
                return;
            }
        }
        args.GetReturnValue().Set(false);
    };
    (void)duplex_instance->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::Function::New(context, proxy_write).ToLocalChecked());
    
    // Proxy 'data' and 'end' from the last stream to this duplex proxy
    v8::Local<v8::Value> on_fn_val;
    if (last_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_fn_val) && on_fn_val->IsFunction()) {
        v8::Local<v8::Function> last_on = on_fn_val.As<v8::Function>();
        
        auto fwd_event_helper = [](v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Object> last_stream, v8::Local<v8::Function> last_on, v8::Local<v8::Object> proxy, const char* event_name) {
            v8::Local<v8::Object> data = v8::Object::New(p_isolate);
            (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "proxy"), proxy);
            (void)data->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "event"), v8::String::NewFromUtf8(p_isolate, event_name).ToLocalChecked());

            auto handler = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                v8::Isolate* p_iso = args.GetIsolate();
                v8::Local<v8::Context> context = p_iso->GetCurrentContext();
                v8::Local<v8::Object> data = args.Data().As<v8::Object>();
                
                v8::Local<v8::Object> proxy = data->Get(context, v8::String::NewFromUtf8Literal(p_iso, "proxy")).ToLocalChecked().As<v8::Object>();
                v8::Local<v8::String> event = data->Get(context, v8::String::NewFromUtf8Literal(p_iso, "event")).ToLocalChecked().As<v8::String>();
                
                v8::Local<v8::Value> emit_fn_val;
                if (proxy->Get(context, v8::String::NewFromUtf8Literal(p_iso, "emit")).ToLocal(&emit_fn_val) && emit_fn_val->IsFunction()) {
                    std::vector<v8::Local<v8::Value>> emit_argv;
                    emit_argv.push_back(event);
                    for (int32_t i = 0; i < args.Length(); ++i) emit_argv.push_back(args[i]);
                    (void)emit_fn_val.As<v8::Function>()->Call(context, proxy, emit_argv.size(), emit_argv.data());
                }
            };
            
            v8::Local<v8::Function> handler_fn = v8::Function::New(context, handler, data).ToLocalChecked();
            v8::Local<v8::Value> on_argv[] = { v8::String::NewFromUtf8(p_isolate, event_name).ToLocalChecked(), handler_fn };
            (void)last_on->Call(context, last_stream, 2, on_argv);
        };
        
        fwd_event_helper(p_isolate, context, last_stream, last_on, duplex_instance, "data");
        fwd_event_helper(p_isolate, context, last_stream, last_on, duplex_instance, "end");
        fwd_event_helper(p_isolate, context, last_stream, last_on, duplex_instance, "error");
    }
    
    args.GetReturnValue().Set(duplex_instance);
}

void Stream::addAbortSignal(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 2 || !args[0]->IsObject() || !args[1]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Signal and stream arguments required")));
        return;
    }
    
    v8::Local<v8::Object> signal = args[0].As<v8::Object>();
    v8::Local<v8::Object> stream_obj = args[1].As<v8::Object>();
    
    // Listen for 'abort' event on signal
    v8::Local<v8::Value> add_listener_fn;
    if (signal->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "addEventListener")).ToLocal(&add_listener_fn) && add_listener_fn->IsFunction()) {
        v8::Local<v8::Function> abort_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_isolate = cb_args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Object> stream_obj = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> destroy_fn;
            if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "destroy")).ToLocal(&destroy_fn) && destroy_fn->IsFunction()) {
                v8::Local<v8::Value> error = v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "AbortError"));
                v8::Local<v8::Value> argv[] = { error };
                (void)destroy_fn.As<v8::Function>()->Call(context, stream_obj, 1, argv);
            }
        }, stream_obj).ToLocalChecked();
        
        v8::Local<v8::Value> listener_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "abort"), 
            abort_handler 
        };
        (void)add_listener_fn.As<v8::Function>()->Call(context, signal, 2, listener_argv);
    }
    
    args.GetReturnValue().Set(stream_obj);
}

static uint32_t s_default_high_water_mark = 16 * 1024; // 16KB
static uint32_t s_default_object_mode_high_water_mark = 16;

void Stream::getDefaultHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    bool object_mode = false;
    if (args.Length() > 0) {
        object_mode = args[0]->BooleanValue(p_isolate);
    }
    args.GetReturnValue().Set(object_mode ? s_default_object_mode_high_water_mark : s_default_high_water_mark);
}

void Stream::setDefaultHighWaterMark(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsNumber()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "value argument required and must be a number")));
        return;
    }
    uint32_t val = args[0]->Uint32Value(p_isolate->GetCurrentContext()).FromMaybe(16384);
    bool object_mode = false;
    if (args.Length() > 1) {
        object_mode = args[1]->BooleanValue(p_isolate);
    }
    
    if (object_mode) {
        s_default_object_mode_high_water_mark = val;
    } else {
        s_default_high_water_mark = val;
    }
}


// --- Utilities ---

void Stream::isErrored(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(false);
        return;
    }
    v8::Local<v8::Object> obj = args[0].As<v8::Object>();
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = obj->GetInternalField(0).As<v8::External>();
        if (!ext.IsEmpty() && ext->Value()) {
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            args.GetReturnValue().Set(p_internal->m_errored);
            return;
        }
    }
    v8::Local<v8::Value> err_val;
    if (obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "errored")).ToLocal(&err_val) && err_val->IsTrue()) {
         args.GetReturnValue().Set(true);
         return;
    }
    args.GetReturnValue().Set(false);
}

void Stream::isReadable(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(false);
        return;
    }
    v8::Local<v8::Object> obj = args[0].As<v8::Object>();
    v8::Local<v8::Value> readable_val;
    if (obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "readable")).ToLocal(&readable_val)) {
        args.GetReturnValue().Set(readable_val->BooleanValue(args.GetIsolate()));
        return;
    }
    args.GetReturnValue().Set(false);
}

void Stream::isDisturbed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(false);
        return;
    }
    v8::Local<v8::Object> obj = args[0].As<v8::Object>();
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = obj->GetInternalField(0).As<v8::External>();
        if (!ext.IsEmpty() && ext->Value()) {
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            args.GetReturnValue().Set(p_internal->m_flowing || p_internal->m_bytes_read > 0 || p_internal->m_ended || p_internal->m_closed);
            return;
        }
    }
    args.GetReturnValue().Set(false);
}

void Stream::destroy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(args.This());
        return;
    }
    v8::Local<v8::Object> obj = args[0].As<v8::Object>();
    v8::Local<v8::Value> destroy_val;
    if (obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "destroy")).ToLocal(&destroy_val) && destroy_val->IsFunction()) {
        (void)destroy_val.As<v8::Function>()->Call(args.GetIsolate()->GetCurrentContext(), obj, 0, nullptr);
    }
    args.GetReturnValue().Set(obj);
}

// Additional Stream implementations - to be appended to stream.cpp

// --- PassThrough Stream ---

v8::Local<v8::FunctionTemplate> Stream::getPassThroughTemplate(v8::Isolate* p_isolate) {
    if (m_passthrough_tmpl.IsEmpty()) {
        v8::Local<v8::FunctionTemplate> transform_tmpl = getTransformTemplate(p_isolate);
        m_passthrough_tmpl.Reset(p_isolate, createPassThroughTemplate(p_isolate, transform_tmpl));
    }
    return m_passthrough_tmpl.Get(p_isolate);
}

v8::Local<v8::FunctionTemplate> Stream::createPassThroughTemplate(v8::Isolate* p_isolate, v8::Local<v8::FunctionTemplate> transform_tmpl) {
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, passThroughConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "PassThrough"));
    tmpl->Inherit(transform_tmpl);
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    return tmpl;
}

void Stream::passThroughConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    // Call transform constructor first
    transformConstructor(args);
    
    v8::Local<v8::Object> self = args.This();
    
    // Override write to directly emit 'data' events for passthrough behavior
    auto write_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        if (args.Length() >= 1) {
            // Emit 'data' event with the chunk
            v8::Local<v8::Value> emit_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                v8::Local<v8::Value> emit_argv[] = {
                    v8::String::NewFromUtf8Literal(p_isolate, "data"),
                    args[0]
                };
                (void)emit_fn->Call(context, self, 2, emit_argv);
            }
        }
        
        args.GetReturnValue().Set(true);
    };
    
    v8::Local<v8::Function> write_func = v8::Function::New(context, write_fn).ToLocalChecked();
    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), write_func);
    
    // Override end to emit 'end' event
    auto end_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        // If there's a chunk argument, write it first
        if (args.Length() >= 1 && !args[0]->IsUndefined()) {
            v8::Local<v8::Value> write_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_val) && write_val->IsFunction()) {
                v8::Local<v8::Function> write_fn = write_val.As<v8::Function>();
                v8::Local<v8::Value> write_argv[] = { args[0] };
                (void)write_fn->Call(context, self, 1, write_argv);
            }
        }
        
        // Emit 'end' event
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
            v8::Local<v8::Value> emit_argv[] = {
                v8::String::NewFromUtf8Literal(p_isolate, "end")
            };
            (void)emit_fn->Call(context, self, 1, emit_argv);
        }
        
        // Mark as ended
        v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
        StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
        p_internal->m_ended = true;
    };
    
    v8::Local<v8::Function> end_func = v8::Function::New(context, end_fn).ToLocalChecked();
    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "end"), end_func);
}

// --- Duplex.from ---

void Stream::duplexFrom(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Argument required for Duplex.from")));
        return;
    }
    
    v8::Local<v8::Value> source = args[0];
    
    // Create a basic Duplex instance
    v8::Local<v8::FunctionTemplate> duplex_tmpl = getDuplexTemplate(p_isolate);
    v8::Local<v8::Function> duplex_ctor;
    if (!duplex_tmpl->GetFunction(context).ToLocal(&duplex_ctor)) {
        return;
    }
    
    v8::Local<v8::Object> duplex_instance;
    if (!duplex_ctor->NewInstance(context, 0, nullptr).ToLocal(&duplex_instance)) {
        return;
    }
    
    // If source is iterable (has Symbol.iterator), set up read from it
    v8::Local<v8::Value> iterator_symbol = v8::Symbol::GetIterator(p_isolate);
    if (source->IsObject()) {
        v8::Local<v8::Object> source_obj = source.As<v8::Object>();
        v8::Local<v8::Value> iterator_fn;
        if (source_obj->Get(context, iterator_symbol).ToLocal(&iterator_fn) && iterator_fn->IsFunction()) {
            // Store the iterator for later use in _read
            (void)duplex_instance->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_sourceIterator"), source);
        }
    }
    
    args.GetReturnValue().Set(duplex_instance);
}

// --- Additional Property Getters ---

void Stream::getReadableClosed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_closed);
}

void Stream::getReadableDestroyed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_destroyed);
}

void Stream::getReadableErrored(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_errored ? v8::True(p_isolate) : v8::Null(p_isolate));
}

void Stream::getWritableClosed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_closed);
}

void Stream::getWritableDestroyed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_destroyed);
}

void Stream::getWritableErrored(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_errored ? v8::True(p_isolate) : v8::Null(p_isolate));
}

// --- Readable Collection Methods ---

void Stream::readableMap(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }

    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();

    // Create a new Readable that will emit mapped data
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor;
    if (!readable_tmpl->GetFunction(context).ToLocal(&readable_ctor)) {
        return;
    }

    v8::Local<v8::Object> result_stream;
    if (!readable_ctor->NewInstance(context, 0, nullptr).ToLocal(&result_stream)) {
        return;
    }

    // Create a V8-managed state object
    v8::Local<v8::Object> state = v8::Object::New(p_isolate);
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "map_fn"), fn);
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "result_stream"), result_stream);
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, false));
    
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // data handler: map then push() into result_stream
        v8::Local<v8::Function> data_handler = v8::Function::New(context,
            [](const v8::FunctionCallbackInfo<v8::Value>& cb) {
                v8::Isolate* p_iso = cb.GetIsolate();
                v8::Local<v8::Context> ctx = p_iso->GetCurrentContext();
                v8::Local<v8::Object> state = cb.Data().As<v8::Object>();
                
                v8::Local<v8::Value> done_val;
                if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_iso)) return;
                
                v8::Local<v8::Function> map_fn = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "map_fn")).ToLocalChecked().As<v8::Function>();
                v8::Local<v8::Object>   res    = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "result_stream")).ToLocalChecked().As<v8::Object>();
                
                v8::Local<v8::Value> fn_argv[] = { cb[0] };
                v8::Local<v8::Value> mapped;
                if (!map_fn->Call(ctx, v8::Undefined(p_iso), 1, fn_argv).ToLocal(&mapped)) {
                    return;
                }
                
                v8::Local<v8::Value> push_fn_val;
                if (res->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "push")).ToLocal(&push_fn_val) && push_fn_val->IsFunction()) {
                    v8::Local<v8::Value> push_argv[] = { mapped };
                    (void)push_fn_val.As<v8::Function>()->Call(ctx, res, 1, push_argv);
                }
            }, state).ToLocalChecked();
            
        v8::Local<v8::Value> data_on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "data"), data_handler
        };
        (void)on_fn->Call(context, self, 2, data_on_argv);
        
        // end handler: push(null) to close result_stream, then clean up
        v8::Local<v8::Function> end_handler = v8::Function::New(context,
            [](const v8::FunctionCallbackInfo<v8::Value>& cb) {
                v8::Isolate* p_iso = cb.GetIsolate();
                v8::Local<v8::Context> ctx = p_iso->GetCurrentContext();
                v8::Local<v8::Object> state = cb.Data().As<v8::Object>();
                
                v8::Local<v8::Value> done_val;
                if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_iso)) return;
                (void)state->Set(ctx, v8::String::NewFromUtf8Literal(p_iso, "done"), v8::Boolean::New(p_iso, true));
                
                v8::Local<v8::Object> res = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "result_stream")).ToLocalChecked().As<v8::Object>();
                v8::Local<v8::Value> push_fn_val;
                if (res->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "push")).ToLocal(&push_fn_val) && push_fn_val->IsFunction()) {
                    v8::Local<v8::Value> null_argv[] = { v8::Null(p_iso) };
                    (void)push_fn_val.As<v8::Function>()->Call(ctx, res, 1, null_argv);
                }
            }, state).ToLocalChecked();
            
        v8::Local<v8::Value> end_on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"), end_handler
        };
        (void)on_fn->Call(context, self, 2, end_on_argv);
        
        // error handler: forward error to result_stream
        v8::Local<v8::Function> error_handler = v8::Function::New(context,
            [](const v8::FunctionCallbackInfo<v8::Value>& cb) {
                v8::Isolate* p_isolate = cb.GetIsolate();
                v8::Local<v8::Context> ctx = p_isolate->GetCurrentContext();
                v8::Local<v8::Object> state = cb.Data().As<v8::Object>();
                
                v8::Local<v8::Value> done_val;
                if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_isolate, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_isolate)) return;
                (void)state->Set(ctx, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
                
                v8::Local<v8::Object> res = state->Get(ctx, v8::String::NewFromUtf8Literal(p_isolate, "result_stream")).ToLocalChecked().As<v8::Object>();
                v8::Local<v8::Value> destroy_fn_val;
                if (res->Get(ctx, v8::String::NewFromUtf8Literal(p_isolate, "destroy")).ToLocal(&destroy_fn_val) && destroy_fn_val->IsFunction()) {
                    v8::Local<v8::Value> err = cb.Length() > 0 ? cb[0] : v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Stream error in map"));
                    v8::Local<v8::Value> destroy_argv[] = { err };
                    (void)destroy_fn_val.As<v8::Function>()->Call(ctx, res, 1, destroy_argv);
                }
            }, state).ToLocalChecked();
            
        v8::Local<v8::Value> error_on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "error"), error_handler
        };
        (void)on_fn->Call(context, self, 2, error_on_argv);
    }
    
    args.GetReturnValue().Set(result_stream);
}

void Stream::readableFilter(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a new Readable that will emit filtered data
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor;
    if (!readable_tmpl->GetFunction(context).ToLocal(&readable_ctor)) {
        return;
    }
    
    v8::Local<v8::Object> result_stream;
    if (!readable_ctor->NewInstance(context, 0, nullptr).ToLocal(&result_stream)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Object>* p_result = new v8::Global<v8::Object>(p_isolate, result_stream);
    
    // Listen to source 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>*>(ext->Value());
            
            v8::Local<v8::Function> fn = p_data->first->Get(p_isolate);
            v8::Local<v8::Object> result_stream = p_data->second->Get(p_isolate);
            
            // Apply filter function
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            v8::Local<v8::Value> filter_result;
            if (fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv).ToLocal(&filter_result) && filter_result->BooleanValue(p_isolate)) {
                // Emit data on result stream if filter passes
                v8::Local<v8::Value> emit_val;
                if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                    v8::Local<v8::Value> emit_argv[] = {
                        v8::String::NewFromUtf8Literal(p_isolate, "data"),
                        args[0]
                    };
                    (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                }
            }
        };
        
        auto* p_callback_data = new std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>(p_fn, p_result);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // Forward 'end' event
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = p_data->second->Get(p_isolate);
            
            v8::Local<v8::Value> emit_val;
            if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                v8::Local<v8::Value> emit_argv[] = {
                    v8::String::NewFromUtf8Literal(p_isolate, "end")
                };
                (void)emit_fn->Call(context, result_stream, 1, emit_argv);
            }
            
            delete p_data->first;
            delete p_data->second;
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(result_stream);
}

void Stream::readableForEach(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that resolves when forEach completes
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Promise::Resolver>* p_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
    
    // Listen for 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            v8::Global<v8::Function>* p_fn = static_cast<v8::Global<v8::Function>*>(ext->Value());
            
            v8::Local<v8::Function> fn = p_fn->Get(p_isolate);
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            (void)fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv);
        };
        
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_fn);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // End handler
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::pair<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*>*>(ext->Value());
            
            v8::Local<v8::Promise::Resolver> resolver = p_data->second->Get(p_isolate);
            (void)resolver->Resolve(context, v8::Undefined(p_isolate));
            
            delete p_data->first;
            delete p_data->second;
            delete p_data;
        };
        
        auto* p_end_data = new std::pair<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*>(p_fn, p_resolver);
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_end_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

void Stream::readableToArray(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that will resolve with an array of all chunks
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    // Create a V8-managed state object
    v8::Local<v8::Object> state = v8::Object::New(p_isolate);
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "chunks"), v8::Array::New(p_isolate));
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "resolver"), resolver);
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "index"), v8::Integer::New(p_isolate, 0));
    (void)state->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, false));
    
    // Listen for events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        v8::Local<v8::Function> data_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_iso = cb_args.GetIsolate();
            v8::Local<v8::Context> ctx = p_iso->GetCurrentContext();
            v8::Local<v8::Object> state = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> done_val;
            if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_iso)) return;
            
            v8::Local<v8::Array> chunks = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "chunks")).ToLocalChecked().As<v8::Array>();
            uint32_t index = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "index")).ToLocalChecked()->Uint32Value(ctx).ToChecked();
            (void)chunks->Set(ctx, index, cb_args[0]);
            (void)state->Set(ctx, v8::String::NewFromUtf8Literal(p_iso, "index"), v8::Integer::New(p_iso, index + 1));
        }, state).ToLocalChecked();
        
        // End handler
        v8::Local<v8::Function> end_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_iso = cb_args.GetIsolate();
            v8::Local<v8::Context> ctx = p_iso->GetCurrentContext();
            v8::Local<v8::Object> state = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> done_val;
            if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_iso)) return;
            (void)state->Set(ctx, v8::String::NewFromUtf8Literal(p_iso, "done"), v8::Boolean::New(p_iso, true));
            
            v8::Local<v8::Promise::Resolver> resolver = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "resolver")).ToLocalChecked().As<v8::Promise::Resolver>();
            v8::Local<v8::Array> chunks = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "chunks")).ToLocalChecked().As<v8::Array>();
            (void)resolver->Resolve(ctx, chunks);
        }, state).ToLocalChecked();
        
        // Error handler
        v8::Local<v8::Function> error_handler = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Isolate* p_iso = cb_args.GetIsolate();
            v8::Local<v8::Context> ctx = p_iso->GetCurrentContext();
            v8::Local<v8::Object> state = cb_args.Data().As<v8::Object>();
            
            v8::Local<v8::Value> done_val;
            if (state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_iso)) return;
            (void)state->Set(ctx, v8::String::NewFromUtf8Literal(p_iso, "done"), v8::Boolean::New(p_iso, true));
            
            v8::Local<v8::Promise::Resolver> resolver = state->Get(ctx, v8::String::NewFromUtf8Literal(p_iso, "resolver")).ToLocalChecked().As<v8::Promise::Resolver>();
            v8::Local<v8::Value> err = cb_args.Length() > 0 ? cb_args[0] : v8::Exception::Error(v8::String::NewFromUtf8Literal(p_iso, "Stream error in toArray"));
            (void)resolver->Reject(ctx, err);
        }, state).ToLocalChecked();
        
        v8::Local<v8::Value> data_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), data_handler };
        (void)on_fn->Call(context, self, 2, data_argv);
        
        v8::Local<v8::Value> end_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end"), end_handler };
        (void)on_fn->Call(context, self, 2, end_argv);
        
        v8::Local<v8::Value> error_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), error_handler };
        (void)on_fn->Call(context, self, 2, error_argv);
        
        // Force flow
        v8::Local<v8::Value> resume_fn;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resume")).ToLocal(&resume_fn) && resume_fn->IsFunction()) {
            (void)resume_fn.As<v8::Function>()->Call(context, self, 0, nullptr);
        }
    } else {
        v8::Local<v8::Array> empty_arr = v8::Array::New(p_isolate);
        (void)resolver->Resolve(context, empty_arr);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

void Stream::readableSome(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that resolves with boolean
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Promise::Resolver>* p_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
    bool* p_found = new bool(false);
    
    // Listen for 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (*std::get<2>(*p_data)) return; // Already found
            
            v8::Local<v8::Function> fn = std::get<0>(*p_data)->Get(p_isolate);
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            v8::Local<v8::Value> result;
            if (fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv).ToLocal(&result) && result->BooleanValue(p_isolate)) {
                *std::get<2>(*p_data) = true;
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, v8::Boolean::New(p_isolate, true));
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>(p_fn, p_resolver, p_found);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // End handler
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (!*std::get<2>(*p_data)) {
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, v8::Boolean::New(p_isolate, false));
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

void Stream::readableFind(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that resolves with the found value or undefined
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Promise::Resolver>* p_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
    bool* p_found = new bool(false);
    
    // Listen for 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (*std::get<2>(*p_data)) return; // Already found
            
            v8::Local<v8::Function> fn = std::get<0>(*p_data)->Get(p_isolate);
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            v8::Local<v8::Value> result;
            if (fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv).ToLocal(&result) && result->BooleanValue(p_isolate)) {
                *std::get<2>(*p_data) = true;
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, args[0]);
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>(p_fn, p_resolver, p_found);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // End handler
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (!*std::get<2>(*p_data)) {
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, v8::Undefined(p_isolate));
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

void Stream::readableEvery(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that resolves with boolean
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Promise::Resolver>* p_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
    bool* p_failed = new bool(false);
    
    // Listen for 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (*std::get<2>(*p_data)) return; // Already failed
            
            v8::Local<v8::Function> fn = std::get<0>(*p_data)->Get(p_isolate);
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            v8::Local<v8::Value> result;
            if (fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv).ToLocal(&result) && !result->BooleanValue(p_isolate)) {
                *std::get<2>(*p_data) = true;
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, v8::Boolean::New(p_isolate, false));
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>(p_fn, p_resolver, p_failed);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // End handler
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            if (!*std::get<2>(*p_data)) {
                v8::Local<v8::Promise::Resolver> resolver = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, v8::Boolean::New(p_isolate, true));
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

void Stream::readableFlatMap(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a new Readable that will emit flattened mapped data
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor;
    if (!readable_tmpl->GetFunction(context).ToLocal(&readable_ctor)) {
        return;
    }
    
    v8::Local<v8::Object> result_stream;
    if (!readable_ctor->NewInstance(context, 0, nullptr).ToLocal(&result_stream)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Object>* p_result = new v8::Global<v8::Object>(p_isolate, result_stream);
    
    // Listen to source 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>*>(ext->Value());
            
            v8::Local<v8::Function> fn = p_data->first->Get(p_isolate);
            v8::Local<v8::Object> result_stream = p_data->second->Get(p_isolate);
            
            // Apply flatMap function
            v8::Local<v8::Value> fn_argv[] = { args[0] };
            v8::Local<v8::Value> mapped_value;
            if (fn->Call(context, v8::Undefined(p_isolate), 1, fn_argv).ToLocal(&mapped_value)) {
                // Flatten the result
                v8::Local<v8::Value> emit_val;
                if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                    
                    // Check if result is an array
                    if (mapped_value->IsArray()) {
                        v8::Local<v8::Array> arr = mapped_value.As<v8::Array>();
                        uint32_t length = arr->Length();
                        
                        // Emit each element of the array
                        for (uint32_t i = 0; i < length; i++) {
                            v8::Local<v8::Value> element;
                            if (arr->Get(context, i).ToLocal(&element)) {
                                v8::Local<v8::Value> emit_argv[] = {
                                    v8::String::NewFromUtf8Literal(p_isolate, "data"),
                                    element
                                };
                                (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                            }
                        }
                    } else if (mapped_value->IsObject()) {
                        // Check if it's iterable (has Symbol.iterator)
                        v8::Local<v8::Object> obj = mapped_value.As<v8::Object>();
                        v8::Local<v8::Value> iterator_symbol = v8::Symbol::GetIterator(p_isolate);
                        v8::Local<v8::Value> iterator_fn;
                        
                        if (obj->Get(context, iterator_symbol).ToLocal(&iterator_fn) && iterator_fn->IsFunction()) {
                            // Get iterator
                            v8::Local<v8::Function> iter_fn = iterator_fn.As<v8::Function>();
                            v8::Local<v8::Value> iterator;
                            if (iter_fn->Call(context, obj, 0, nullptr).ToLocal(&iterator) && iterator->IsObject()) {
                                v8::Local<v8::Object> iter_obj = iterator.As<v8::Object>();
                                v8::Local<v8::Value> next_fn_val;
                                
                                if (iter_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "next")).ToLocal(&next_fn_val) && next_fn_val->IsFunction()) {
                                    v8::Local<v8::Function> next_fn = next_fn_val.As<v8::Function>();
                                    
                                    // Iterate through all values
                                    while (true) {
                                        v8::Local<v8::Value> next_result;
                                        if (!next_fn->Call(context, iter_obj, 0, nullptr).ToLocal(&next_result) || !next_result->IsObject()) {
                                            break;
                                        }
                                        
                                        v8::Local<v8::Object> result_obj = next_result.As<v8::Object>();
                                        v8::Local<v8::Value> done_val;
                                        if (result_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_isolate)) {
                                            break;
                                        }
                                        
                                        v8::Local<v8::Value> value;
                                        if (result_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "value")).ToLocal(&value)) {
                                            v8::Local<v8::Value> emit_argv[] = {
                                                v8::String::NewFromUtf8Literal(p_isolate, "data"),
                                                value
                                            };
                                            (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                                        }
                                    }
                                } else {
                                    // Not iterable, emit as-is
                                    v8::Local<v8::Value> emit_argv[] = {
                                        v8::String::NewFromUtf8Literal(p_isolate, "data"),
                                        mapped_value
                                    };
                                    (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                                }
                            } else {
                                // Iterator call failed, emit as-is
                                v8::Local<v8::Value> emit_argv[] = {
                                    v8::String::NewFromUtf8Literal(p_isolate, "data"),
                                    mapped_value
                                };
                                (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                            }
                        } else {
                            // Not iterable, emit as-is
                            v8::Local<v8::Value> emit_argv[] = {
                                v8::String::NewFromUtf8Literal(p_isolate, "data"),
                                mapped_value
                            };
                            (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                        }
                    } else {
                        // Not an array or object, emit as-is
                        v8::Local<v8::Value> emit_argv[] = {
                            v8::String::NewFromUtf8Literal(p_isolate, "data"),
                            mapped_value
                        };
                        (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                    }
                }
            }
        };
        
        auto* p_callback_data = new std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>(p_fn, p_result);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // Forward 'end' event
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::pair<v8::Global<v8::Function>*, v8::Global<v8::Object>*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = p_data->second->Get(p_isolate);
            
            v8::Local<v8::Value> emit_val;
            if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                v8::Local<v8::Value> emit_argv[] = {
                    v8::String::NewFromUtf8Literal(p_isolate, "end")
                };
                (void)emit_fn->Call(context, result_stream, 1, emit_argv);
            }
            
            delete p_data->first;
            delete p_data->second;
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(result_stream);
}

void Stream::readableDrop(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsNumber()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Number required for drop limit")));
        return;
    }
    
    int32_t limit = args[0]->Int32Value(context).FromMaybe(0);
    v8::Local<v8::Object> self = args.This();
    
    // Create a new Readable that will emit data after dropping first N chunks
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor;
    if (!readable_tmpl->GetFunction(context).ToLocal(&readable_ctor)) {
        return;
    }
    
    v8::Local<v8::Object> result_stream;
    if (!readable_ctor->NewInstance(context, 0, nullptr).ToLocal(&result_stream)) {
        return;
    }
    
    v8::Global<v8::Object>* p_result = new v8::Global<v8::Object>(p_isolate, result_stream);
    int32_t* p_count = new int32_t(0);
    int32_t* p_limit = new int32_t(limit);
    
    // Listen to source 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = std::get<0>(*p_data)->Get(p_isolate);
            int32_t* p_count = std::get<1>(*p_data);
            int32_t* p_limit = std::get<2>(*p_data);
            
            (*p_count)++;
            
            // Only emit if we've dropped enough chunks
            if (*p_count > *p_limit) {
                v8::Local<v8::Value> emit_val;
                if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                    v8::Local<v8::Value> emit_argv[] = {
                        v8::String::NewFromUtf8Literal(p_isolate, "data"),
                        args[0]
                    };
                    (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                }
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*>(p_result, p_count, p_limit);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // Forward 'end' event
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = std::get<0>(*p_data)->Get(p_isolate);
            
            v8::Local<v8::Value> emit_val;
            if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                v8::Local<v8::Value> emit_argv[] = {
                    v8::String::NewFromUtf8Literal(p_isolate, "end")
                };
                (void)emit_fn->Call(context, result_stream, 1, emit_argv);
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(result_stream);
}

void Stream::readableTake(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsNumber()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Number required for take limit")));
        return;
    }
    
    int32_t limit = args[0]->Int32Value(context).FromMaybe(0);
    v8::Local<v8::Object> self = args.This();
    
    // Create a new Readable that will emit only first N chunks
    v8::Local<v8::FunctionTemplate> readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Function> readable_ctor;
    if (!readable_tmpl->GetFunction(context).ToLocal(&readable_ctor)) {
        return;
    }
    
    v8::Local<v8::Object> result_stream;
    if (!readable_ctor->NewInstance(context, 0, nullptr).ToLocal(&result_stream)) {
        return;
    }
    
    v8::Global<v8::Object>* p_result = new v8::Global<v8::Object>(p_isolate, result_stream);
    int32_t* p_count = new int32_t(0);
    int32_t* p_limit = new int32_t(limit);
    bool* p_ended = new bool(false);
    
    // Listen to source 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*, bool*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = std::get<0>(*p_data)->Get(p_isolate);
            int32_t* p_count = std::get<1>(*p_data);
            int32_t* p_limit = std::get<2>(*p_data);
            bool* p_ended = std::get<3>(*p_data);
            
            if (*p_ended) return;
            
            (*p_count)++;
            
            // Emit if we haven't reached the limit
            if (*p_count <= *p_limit) {
                v8::Local<v8::Value> emit_val;
                if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                    v8::Local<v8::Value> emit_argv[] = {
                        v8::String::NewFromUtf8Literal(p_isolate, "data"),
                        args[0]
                    };
                    (void)emit_fn->Call(context, result_stream, 2, emit_argv);
                }
                
                // If we've reached the limit, emit 'end'
                if (*p_count >= *p_limit) {
                    *p_ended = true;
                    v8::Local<v8::Value> emit_val;
                    if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                        v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                        v8::Local<v8::Value> emit_argv[] = {
                            v8::String::NewFromUtf8Literal(p_isolate, "end")
                        };
                        (void)emit_fn->Call(context, result_stream, 1, emit_argv);
                    }
                }
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*, bool*>(p_result, p_count, p_limit, p_ended);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // Forward 'end' event if not already ended
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Object>*, int32_t*, int32_t*, bool*>*>(ext->Value());
            
            v8::Local<v8::Object> result_stream = std::get<0>(*p_data)->Get(p_isolate);
            bool* p_ended = std::get<3>(*p_data);
            
            if (!*p_ended) {
                v8::Local<v8::Value> emit_val;
                if (result_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                    v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                    v8::Local<v8::Value> emit_argv[] = {
                        v8::String::NewFromUtf8Literal(p_isolate, "end")
                    };
                    (void)emit_fn->Call(context, result_stream, 1, emit_argv);
                }
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete std::get<3>(*p_data);
            delete p_data;
        };
        
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(result_stream);
}

void Stream::readableReduce(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Callback function required")));
        return;
    }
    
    v8::Local<v8::Function> fn = args[0].As<v8::Function>();
    v8::Local<v8::Object> self = args.This();
    
    // Create a promise that resolves with the reduced value
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return;
    }
    
    v8::Global<v8::Function>* p_fn = new v8::Global<v8::Function>(p_isolate, fn);
    v8::Global<v8::Promise::Resolver>* p_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
    
    // Check if initial value is provided
    bool has_initial = args.Length() > 1;
    v8::Global<v8::Value>* p_accumulator = has_initial ? 
        new v8::Global<v8::Value>(p_isolate, args[1]) : 
        new v8::Global<v8::Value>();
    bool* p_first = new bool(!has_initial);
    
    // Listen for 'data' events
    v8::Local<v8::Value> on_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "on")).ToLocal(&on_val) && on_val->IsFunction()) {
        v8::Local<v8::Function> on_fn = on_val.As<v8::Function>();
        
        // Data handler
        auto data_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Value>*, bool*>*>(ext->Value());
            
            if (*std::get<2>(*p_data)) {
                // First chunk without initial value
                *std::get<2>(*p_data) = false;
                std::get<1>(*p_data)->Reset(p_isolate, args[0]);
            } else {
                v8::Local<v8::Function> fn = std::get<0>(*p_data)->Get(p_isolate);
                v8::Local<v8::Value> accumulator = std::get<1>(*p_data)->Get(p_isolate);
                v8::Local<v8::Value> fn_argv[] = { accumulator, args[0] };
                v8::Local<v8::Value> result;
                if (fn->Call(context, v8::Undefined(p_isolate), 2, fn_argv).ToLocal(&result)) {
                    std::get<1>(*p_data)->Reset(p_isolate, result);
                }
            }
        };
        
        auto* p_callback_data = new std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Value>*, bool*>(p_fn, p_accumulator, p_first);
        v8::Local<v8::External> data_ext = v8::External::New(p_isolate, p_callback_data);
        v8::Local<v8::Function> data_handler = v8::Function::New(context, data_callback, data_ext).ToLocalChecked();
        
        v8::Local<v8::Value> on_argv[] = { 
            v8::String::NewFromUtf8Literal(p_isolate, "data"),
            data_handler
        };
        (void)on_fn->Call(context, self, 2, on_argv);
        
        // End handler
        auto end_callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            v8::Isolate* p_isolate = args.GetIsolate();
            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::External> ext = args.Data().As<v8::External>();
            auto* p_data = static_cast<std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Value>*, v8::Global<v8::Promise::Resolver>*, bool*>*>(ext->Value());
            
            v8::Local<v8::Promise::Resolver> resolver = std::get<2>(*p_data)->Get(p_isolate);
            if (!std::get<1>(*p_data)->IsEmpty()) {
                v8::Local<v8::Value> result = std::get<1>(*p_data)->Get(p_isolate);
                (void)resolver->Resolve(context, result);
            } else {
                (void)resolver->Resolve(context, v8::Undefined(p_isolate));
            }
            
            delete std::get<0>(*p_data);
            delete std::get<1>(*p_data);
            delete std::get<2>(*p_data);
            delete std::get<3>(*p_data);
            delete p_data;
        };
        
        auto* p_end_data = new std::tuple<v8::Global<v8::Function>*, v8::Global<v8::Value>*, v8::Global<v8::Promise::Resolver>*, bool*>(
            p_fn, p_accumulator, p_resolver, p_first);
        v8::Local<v8::External> end_ext = v8::External::New(p_isolate, p_end_data);
        v8::Local<v8::Function> end_handler = v8::Function::New(context, end_callback, end_ext).ToLocalChecked();
        
        v8::Local<v8::Value> end_argv[] = {
            v8::String::NewFromUtf8Literal(p_isolate, "end"),
            end_handler
        };
        (void)on_fn->Call(context, self, 2, end_argv);
    }
    
    args.GetReturnValue().Set(resolver->GetPromise());
}

// --- Additional Property Getters ---

void Stream::getReadableAborted(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_aborted);
}

void Stream::getReadableDidRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_did_read);
}

void Stream::getWritableAborted(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    args.GetReturnValue().Set(p_internal->m_aborted);
}

// --- duplexPair Implementation ---

void Stream::duplexPair(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    // Create two Duplex streams that are connected to each other
    v8::Local<v8::FunctionTemplate> duplex_tmpl = getDuplexTemplate(p_isolate);
    v8::Local<v8::Function> duplex_ctor;
    if (!duplex_tmpl->GetFunction(context).ToLocal(&duplex_ctor)) {
        return;
    }
    
    // Get options if provided
    v8::Local<v8::Value> options = args.Length() > 0 ? args[0] : v8::Object::New(p_isolate).As<v8::Value>();
    v8::Local<v8::Value> ctor_args[] = { options };
    
    // Create side A
    v8::Local<v8::Object> side_a;
    if (!duplex_ctor->NewInstance(context, 1, ctor_args).ToLocal(&side_a)) {
        return;
    }
    
    // Create side B
    v8::Local<v8::Object> side_b;
    if (!duplex_ctor->NewInstance(context, 1, ctor_args).ToLocal(&side_b)) {
        return;
    }
    
    // Connect them: when data is written to A, it should be readable from B and vice versa
    // Store references to each other
    (void)side_a->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_pairSide"), side_b);
    (void)side_b->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_pairSide"), side_a);
    
    // Override write methods to forward data to the other side
    auto write_forwarder = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        // Get the paired side
        v8::Local<v8::Value> pair_side_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_pairSide")).ToLocal(&pair_side_val) && pair_side_val->IsObject()) {
            v8::Local<v8::Object> pair_side = pair_side_val.As<v8::Object>();
            
            // Emit 'data' event on the paired side
            v8::Local<v8::Value> emit_val;
            if (pair_side->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Function> emit_fn = emit_val.As<v8::Function>();
                v8::Local<v8::Value> emit_argv[] = {
                    v8::String::NewFromUtf8Literal(p_isolate, "data"),
                    args[0]
                };
                (void)emit_fn->Call(context, pair_side, 2, emit_argv);
            }
        }
        
        // Call callback if provided
        if (args.Length() > 2 && args[2]->IsFunction()) {
            v8::Local<v8::Function> cb = args[2].As<v8::Function>();
            (void)cb->Call(context, self, 0, nullptr);
        }
        
        args.GetReturnValue().Set(true);
    };
    
    v8::Local<v8::Function> write_fn_a = v8::Function::New(context, write_forwarder).ToLocalChecked();
    v8::Local<v8::Function> write_fn_b = v8::Function::New(context, write_forwarder).ToLocalChecked();
    
    (void)side_a->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), write_fn_a);
    (void)side_b->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), write_fn_b);
    
    // Return array with both sides
    v8::Local<v8::Array> result = v8::Array::New(p_isolate, 2);
    (void)result->Set(context, 0, side_a);
    (void)result->Set(context, 1, side_b);
    
    args.GetReturnValue().Set(result);
}

// --- readable.compose() Implementation ---

void Stream::readableCompose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() < 1) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Stream argument required")));
        return;
    }
    
    // Simplified: just pipe self to the destination
    v8::Local<v8::Value> pipe_fn_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "pipe")).ToLocal(&pipe_fn_val) && pipe_fn_val->IsFunction()) {
        v8::Local<v8::Function> pipe_fn = pipe_fn_val.As<v8::Function>();
        v8::Local<v8::Value> pipe_argv[] = { args[0] };
        v8::Local<v8::Value> result;
        if (pipe_fn->Call(context, self, 1, pipe_argv).ToLocal(&result)) {
            args.GetReturnValue().Set(result);
            return;
        }
    }
    
    args.GetReturnValue().Set(args[0]);
}

// --- readable.iterator() Implementation ---

struct StreamResumeTaskData {
    v8::Global<v8::Object> m_stream;
    StreamResumeTaskData(v8::Isolate* p_isolate, v8::Local<v8::Object> stream)
        : m_stream(p_isolate, stream) {}
    ~StreamResumeTaskData() { m_stream.Reset(); }
};

static void StreamResumeTaskRunner(v8::Isolate* p_isolate, v8::Local<v8::Context> context, z8::Task* p_task) {
    v8::HandleScope handle_scope(p_isolate);
    v8::Context::Scope context_scope(context);
    StreamResumeTaskData* p_data = static_cast<StreamResumeTaskData*>(p_task->p_data);
    v8::Local<v8::Object> stream = p_data->m_stream.Get(p_isolate);
    v8::Local<v8::Value> resume_val;
    if (stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resume")).ToLocal(&resume_val) && resume_val->IsFunction()) {
        (void)resume_val.As<v8::Function>()->Call(context, stream, 0, nullptr);
    }
    delete p_data;
}

void Stream::readableIterator(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    // Parse options
    bool destroy_on_return = true;
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> destroy_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "destroyOnReturn")).ToLocal(&destroy_val)) {
            destroy_on_return = destroy_val->BooleanValue(p_isolate);
        }
    }
    
    // Create an async iterator object
    v8::Local<v8::Object> iterator = v8::Object::New(p_isolate);
    
    // Store stream reference and options
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream"), self);
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_destroyOnReturn"), 
        v8::Boolean::New(p_isolate, destroy_on_return));
    
    // Implement next() method
    auto next_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        // Get stream
        v8::Local<v8::Value> stream_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream")).ToLocal(&stream_val) || !stream_val->IsObject()) {
            v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
            v8::Local<v8::Object> result = v8::Object::New(p_isolate);
            (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
            (void)resolver->Resolve(context, result);
            args.GetReturnValue().Set(resolver->GetPromise());
            return;
        }
        
        v8::Local<v8::Object> stream = stream_val.As<v8::Object>();
        
        // Create promise resolver
        v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
        args.GetReturnValue().Set(resolver->GetPromise());
        
        // Store resolver on iterator object to avoid GC while pending
        v8::Global<v8::Promise::Resolver>* p_persistent_resolver = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
        (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::External::New(p_isolate, p_persistent_resolver));

        // Listen for next data event
        v8::Local<v8::Value> once_val;
        if (stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "once")).ToLocal(&once_val) && once_val->IsFunction()) {
            v8::Local<v8::Function> once_fn = once_val.As<v8::Function>();
            
            // Shared handler for resolving the one-time promise
            auto resolve_handler = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                v8::Isolate* p_isolate = args.GetIsolate();
                v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                v8::Local<v8::Object> iterator = args.Data().As<v8::Object>();
                
                v8::Local<v8::Value> ext_val;
                if (!iterator->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver")).ToLocal(&ext_val) || !ext_val->IsExternal()) return;
                
                v8::Global<v8::Promise::Resolver>* p_res = static_cast<v8::Global<v8::Promise::Resolver>*>(ext_val.As<v8::External>()->Value());
                if (!p_res || p_res->IsEmpty()) return;

                // Determine results
                bool is_end = args.Length() == 0 || args[0]->IsUndefined() || args[0]->IsNull();
                v8::Local<v8::Value> value = is_end ? v8::Null(p_isolate).As<v8::Value>() : args[0];
                
                // If it came from 'end' event, args might be empty or not data
                // We identify end event by checking the event name if needed, but here we use simple logic
                // The actual identification is done by which listener called us.
                
                v8::Local<v8::Object> result = v8::Object::New(p_isolate);
                (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "value"), value);
                (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, is_end));
                
                (void)p_res->Get(p_isolate)->Resolve(context, result);
                
                // Cleanup
                p_res->Reset();
                delete p_res;
                (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::Null(p_isolate));
            };

            v8::Local<v8::Function> data_fn = v8::Function::New(context, resolve_handler, self).ToLocalChecked();
            v8::Local<v8::Value> data_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), data_fn };
            (void)once_fn->Call(context, stream, 2, data_argv);

            // For 'end', we need a wrapper to ensure it returns { done: true }
            auto end_handler = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                v8::Isolate* p_isolate = args.GetIsolate();
                v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                v8::Local<v8::Object> iterator = args.Data().As<v8::Object>();
                
                v8::Local<v8::Value> ext_val;
                if (!iterator->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver")).ToLocal(&ext_val) || !ext_val->IsExternal()) return;
                
                v8::Global<v8::Promise::Resolver>* p_res = static_cast<v8::Global<v8::Promise::Resolver>*>(ext_val.As<v8::External>()->Value());
                if (!p_res || p_res->IsEmpty()) return;
                
                v8::Local<v8::Object> result = v8::Object::New(p_isolate);
                (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "value"), v8::Null(p_isolate));
                (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
                
                (void)p_res->Get(p_isolate)->Resolve(context, result);
                
                p_res->Reset();
                delete p_res;
                (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::Null(p_isolate));
            };
            
            v8::Local<v8::Function> end_fn = v8::Function::New(context, end_handler, self).ToLocalChecked();
            v8::Local<v8::Value> end_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end"), end_fn };
            (void)once_fn->Call(context, stream, 2, end_argv);

            // Force flow
            StreamResumeTaskData* p_res_data = new StreamResumeTaskData(p_isolate, stream);
            z8::Task* p_res_task = new z8::Task();
            p_res_task->p_data = p_res_data;
            p_res_task->m_runner = StreamResumeTaskRunner;
            p_res_task->m_is_promise = false;
            z8::TaskQueue::getInstance().enqueue(p_res_task);
        } else {
            // Immediate resolve if can't listen
            v8::Local<v8::Object> result = v8::Object::New(p_isolate);
            (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
            (void)resolver->Resolve(context, result);
            
            p_persistent_resolver->Reset();
            delete p_persistent_resolver;
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::Null(p_isolate));
        }
    };
    
    v8::Local<v8::Function> next_func = v8::Function::New(context, next_fn).ToLocalChecked();
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "next"), next_func);
    
    // Implement return() method
    auto return_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        // Check destroyOnReturn option
        v8::Local<v8::Value> destroy_val;
        bool destroy_on_return = true;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_destroyOnReturn")).ToLocal(&destroy_val)) {
            destroy_on_return = destroy_val->BooleanValue(p_isolate);
        }
        
        if (destroy_on_return) {
            // Get stream and destroy it
            v8::Local<v8::Value> stream_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream")).ToLocal(&stream_val) && stream_val->IsObject()) {
                v8::Local<v8::Object> stream = stream_val.As<v8::Object>();
                v8::Local<v8::Value> destroy_fn_val;
                if (stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "destroy")).ToLocal(&destroy_fn_val) && destroy_fn_val->IsFunction()) {
                    (void)destroy_fn_val.As<v8::Function>()->Call(context, stream, 0, nullptr);
                }
            }
        }
        
        v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
        v8::Local<v8::Object> result = v8::Object::New(p_isolate);
        (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
        (void)resolver->Resolve(context, result);
        args.GetReturnValue().Set(resolver->GetPromise());
    };
    
    v8::Local<v8::Function> return_func = v8::Function::New(context, return_fn).ToLocalChecked();
    (void)iterator->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "return"), return_func);
    
    args.GetReturnValue().Set(iterator);
}

void Stream::readableAsyncIterator(const v8::FunctionCallbackInfo<v8::Value>& args) {
    readableIterator(args);
}

void Stream::readableAsyncDispose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();

    // Call destroy and resolve
    v8::Local<v8::Value> destroy_fn_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "destroy")).ToLocal(&destroy_fn_val) && destroy_fn_val->IsFunction()) {
        (void)destroy_fn_val.As<v8::Function>()->Call(context, self, 0, nullptr);
    }
    
    (void)resolver->Resolve(context, v8::Undefined(p_isolate));
    args.GetReturnValue().Set(resolver->GetPromise());
}

// --- stream.isWritable ---

void Stream::isWritable(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() < 1 || !args[0]->IsObject()) {
        args.GetReturnValue().Set(false);
        return;
    }
    v8::Local<v8::Object> obj = args[0].As<v8::Object>();
    if (obj->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = obj->GetInternalField(0).As<v8::External>();
        if (!ext.IsEmpty() && ext->Value()) {
            StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
            args.GetReturnValue().Set(p_internal->m_is_writable && !p_internal->m_destroyed && !p_internal->m_ended);
            return;
        }
    }
    v8::Local<v8::Value> writable_val;
    if (obj->Get(args.GetIsolate()->GetCurrentContext(), v8::String::NewFromUtf8Literal(args.GetIsolate(), "writable")).ToLocal(&writable_val)) {
        args.GetReturnValue().Set(writable_val->BooleanValue(args.GetIsolate()));
        return;
    }
    args.GetReturnValue().Set(false);
}

// --- duplex.allowHalfOpen ---

void Stream::getDuplexAllowHalfOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    if (self->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
        StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
        args.GetReturnValue().Set(p_internal->m_allow_half_open);
    } else {
        args.GetReturnValue().Set(true);
    }
}

void Stream::setDuplexAllowHalfOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This();
    if (args.Length() < 1 || self->InternalFieldCount() == 0) {
        return;
    }
    v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
    StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
    p_internal->m_allow_half_open = args[0]->BooleanValue(args.GetIsolate());
}

// --- writable[Symbol.asyncDispose] ---

void Stream::writableAsyncDispose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();

    // Call end() then resolve when finish
    v8::Local<v8::Value> end_fn_val;
    if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "end")).ToLocal(&end_fn_val) && end_fn_val->IsFunction()) {
        v8::Local<v8::Function> finish_cb = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
            v8::Local<v8::Promise::Resolver> local_resolver = cb_args.Data().As<v8::Promise::Resolver>();
            (void)local_resolver->Resolve(cb_args.GetIsolate()->GetCurrentContext(), v8::Undefined(cb_args.GetIsolate()));
        }, resolver).ToLocalChecked();

        v8::Local<v8::Value> end_argv[] = { finish_cb };
        (void)end_fn_val.As<v8::Function>()->Call(context, self, 1, end_argv);
    } else {
        (void)resolver->Resolve(context, v8::Undefined(p_isolate));
    }

    args.GetReturnValue().Set(resolver->GetPromise());
}

// --- readable[Symbol.asyncIterator] ---

// --- Readable.isDisturbed (static) ---

void Stream::readableIsDisturbedStatic(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Forwards to the top-level isDisturbed
    isDisturbed(args);
}

// --- transform.destroy ---

void Stream::transformDestroy(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    if (self->InternalFieldCount() > 0) {
        v8::Local<v8::External> ext = self->GetInternalField(0).As<v8::External>();
        StreamInternal* p_internal = static_cast<StreamInternal*>(ext->Value());
        p_internal->m_destroyed = true;
        p_internal->m_closed = true;

        // Call user-defined _destroy if it exists
        v8::Local<v8::Value> destroy_fn_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_destroy")).ToLocal(&destroy_fn_val) && destroy_fn_val->IsFunction()) {
            v8::Local<v8::Value> err = args.Length() > 0 ? args[0] : v8::Null(p_isolate).As<v8::Value>();
            v8::Local<v8::Function> cb = v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& cb_args) {
                // No-op callback
                (void)cb_args;
            }).ToLocalChecked();
            v8::Local<v8::Value> destroy_argv[] = { err, cb };
            (void)destroy_fn_val.As<v8::Function>()->Call(context, self, 2, destroy_argv);
        }

        // If error, emit it
        if (args.Length() > 0 && !args[0]->IsNull() && !args[0]->IsUndefined()) {
            p_internal->m_errored = true;
            v8::Local<v8::Value> emit_val;
            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
                v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "error"), args[0] };
                (void)emit_val.As<v8::Function>()->Call(context, self, 2, argv);
            }
        }

        // Emit 'close'
        v8::Local<v8::Value> emit_val;
        if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "emit")).ToLocal(&emit_val) && emit_val->IsFunction()) {
            v8::Local<v8::Value> argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "close") };
            (void)emit_val.As<v8::Function>()->Call(context, self, 1, argv);
        }
    }

    args.GetReturnValue().Set(self);
}

// --- Web Streams Interop ---

void Stream::webReadableStreamConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    // Basic implementation: we store the underlying Node stream if this is created via toWeb
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> underlying;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying")).ToLocal(&underlying)) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying"), underlying);
        }
    }
}

void Stream::webReadableStreamGetReader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Object> reader = v8::Object::New(p_isolate);
    (void)reader->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream"), self);
    
    auto read_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> reader = args.This();
        v8::Local<v8::Value> stream_val;
        if (!reader->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream")).ToLocal(&stream_val) || !stream_val->IsObject()) return;
        v8::Local<v8::Object> stream_obj = stream_val.As<v8::Object>();
        
        v8::Local<v8::Value> underlying_val;
        if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying")).ToLocal(&underlying_val) && underlying_val->IsObject()) {
            v8::Local<v8::Object> node_stream = underlying_val.As<v8::Object>();
            
            v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
            // Store resolver on reader to avoid use-after-free and handle multiple events safely
            (void)reader->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), resolver);

            // Listen for 'data'/'end' event once
            v8::Local<v8::Value> once_fn_val;
            if (node_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "once")).ToLocal(&once_fn_val) && once_fn_val->IsFunction()) {
                auto data_handler = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                    v8::Isolate* p_isolate = args.GetIsolate();
                    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                    v8::Local<v8::Object> reader = args.Data().As<v8::Object>();
                    
                    v8::Local<v8::Value> res_val;
                    if (reader->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver")).ToLocal(&res_val) && res_val->IsObject() && !res_val->IsUndefined()) {
                        (void)reader->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::Undefined(p_isolate));
                        
                        v8::Local<v8::Object> result = v8::Object::New(p_isolate);
                        (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "value"), args[0]);
                        (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, false));
                        (void)res_val.As<v8::Promise::Resolver>()->Resolve(context, result);
                    }
                };

                auto end_handler = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                    v8::Isolate* p_isolate = args.GetIsolate();
                    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                    v8::Local<v8::Object> reader = args.Data().As<v8::Object>();
                    
                    v8::Local<v8::Value> res_val;
                    if (reader->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver")).ToLocal(&res_val) && res_val->IsObject() && !res_val->IsUndefined()) {
                        (void)reader->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_resolver"), v8::Undefined(p_isolate));
                        
                        v8::Local<v8::Object> result = v8::Object::New(p_isolate);
                        (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "value"), v8::Null(p_isolate));
                        (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "done"), v8::Boolean::New(p_isolate, true));
                        (void)res_val.As<v8::Promise::Resolver>()->Resolve(context, result);
                    }
                };

                v8::Local<v8::Function> data_fn = v8::Function::New(context, data_handler, reader).ToLocalChecked();
                v8::Local<v8::Function> end_fn = v8::Function::New(context, end_handler, reader).ToLocalChecked();

                v8::Local<v8::Value> data_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "data"), data_fn };
                (void)once_fn_val.As<v8::Function>()->Call(context, node_stream, 2, data_argv);

                v8::Local<v8::Value> end_argv[] = { v8::String::NewFromUtf8Literal(p_isolate, "end"), end_fn };
                (void)once_fn_val.As<v8::Function>()->Call(context, node_stream, 2, end_argv);

                v8::Local<v8::Value> resume_fn_val;
                if (node_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "resume")).ToLocal(&resume_fn_val) && resume_fn_val->IsFunction()) {
                    (void)resume_fn_val.As<v8::Function>()->Call(context, node_stream, 0, nullptr);
                }
            }
            
            args.GetReturnValue().Set(resolver->GetPromise());
            return;
        }
        
        args.GetReturnValue().Set(v8::Promise::Resolver::New(context).ToLocalChecked()->GetPromise());
    };
    
    (void)reader->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "read"), v8::Function::New(context, read_fn).ToLocalChecked());
    args.GetReturnValue().Set(reader);
}

void Stream::webWritableStreamConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> options = args[0].As<v8::Object>();
        v8::Local<v8::Value> underlying;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying")).ToLocal(&underlying)) {
            (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying"), underlying);
        }
    }
}

void Stream::webWritableStreamGetWriter(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Object> writer = v8::Object::New(p_isolate);
    (void)writer->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream"), self);
    
    auto write_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> writer = args.This();
        
        v8::Local<v8::Value> stream_val;
        if (!writer->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_stream")).ToLocal(&stream_val) || !stream_val->IsObject()) return;
        v8::Local<v8::Object> stream_obj = stream_val.As<v8::Object>();
        
        v8::Local<v8::Value> underlying_val;
        if (stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying")).ToLocal(&underlying_val) && underlying_val->IsObject()) {
            v8::Local<v8::Object> node_stream = underlying_val.As<v8::Object>();
            
            if (args.Length() > 0) {
                v8::Local<v8::Value> chunk = args[0];
                v8::Local<v8::Value> write_fn_val;
                if (node_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_fn_val) && write_fn_val->IsFunction()) {
                    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
                    v8::Global<v8::Promise::Resolver>* p_res = new v8::Global<v8::Promise::Resolver>(p_isolate, resolver);
                    
                    auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                        v8::Isolate* p_isolate = args.GetIsolate();
                        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                        v8::Global<v8::Promise::Resolver>* p_res = static_cast<v8::Global<v8::Promise::Resolver>*>(args.Data().As<v8::External>()->Value());
                        (void)p_res->Get(p_isolate)->Resolve(context, v8::Undefined(p_isolate));
                        p_res->Reset();
                        delete p_res;
                    };
                    
                    v8::Local<v8::Value> cb_argv[] = { chunk, v8::Undefined(p_isolate), v8::Function::New(context, callback, v8::External::New(p_isolate, p_res)).ToLocalChecked() };
                    (void)write_fn_val.As<v8::Function>()->Call(context, node_stream, 3, cb_argv);
                    
                    args.GetReturnValue().Set(resolver->GetPromise());
                    return;
                }
            }
        }
        args.GetReturnValue().Set(v8::Promise::Resolver::New(context).ToLocalChecked()->GetPromise());
    };
    
    (void)writer->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::Function::New(context, write_fn).ToLocalChecked());
    args.GetReturnValue().Set(writer);
}

v8::Local<v8::FunctionTemplate> Stream::createWebReadableStreamTemplate(v8::Isolate* p_isolate) {
    if (!m_web_readable_tmpl.IsEmpty()) {
        return m_web_readable_tmpl.Get(p_isolate);
    }
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, webReadableStreamConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "ReadableStream"));
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "getReader"), v8::FunctionTemplate::New(p_isolate, webReadableStreamGetReader));
    m_web_readable_tmpl.Reset(p_isolate, tmpl);
    return tmpl;
}

v8::Local<v8::FunctionTemplate> Stream::createWebWritableStreamTemplate(v8::Isolate* p_isolate) {
    if (!m_web_writable_tmpl.IsEmpty()) {
        return m_web_writable_tmpl.Get(p_isolate);
    }
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(p_isolate, webWritableStreamConstructor);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(p_isolate, "WritableStream"));
    v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();
    proto->Set(v8::String::NewFromUtf8Literal(p_isolate, "getWriter"), v8::FunctionTemplate::New(p_isolate, webWritableStreamGetWriter));
    m_web_writable_tmpl.Reset(p_isolate, tmpl);
    return tmpl;
}

void Stream::readableFromWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() == 0 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "ReadableStream required")));
        return;
    }
    
    v8::Local<v8::Object> web_stream = args[0].As<v8::Object>();
    
    // Create Node Readable using the cached template
    v8::Local<v8::FunctionTemplate> node_readable_tmpl = getReadableTemplate(p_isolate);
    v8::Local<v8::Object> node_readable = node_readable_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    
    // Wrap web_stream into node_readable
    (void)node_readable->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_webStream"), web_stream);
    
    auto read_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        
        v8::Local<v8::Value> web_stream_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_webStream")).ToLocal(&web_stream_val) || !web_stream_val->IsObject()) return;
        v8::Local<v8::Object> web_stream = web_stream_val.As<v8::Object>();
        
        v8::Local<v8::Value> reader_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_reader")).ToLocal(&reader_val) || !reader_val->IsObject()) {
            v8::Local<v8::Value> get_reader_fn;
            if (web_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "getReader")).ToLocal(&get_reader_fn) && get_reader_fn->IsFunction()) {
                if (get_reader_fn.As<v8::Function>()->Call(context, web_stream, 0, nullptr).ToLocal(&reader_val)) {
                    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_reader"), reader_val);
                }
            }
        }
        
        if (!reader_val.IsEmpty() && reader_val->IsObject()) {
            v8::Local<v8::Object> reader = reader_val.As<v8::Object>();
            v8::Local<v8::Value> read_fn_val;
            if (reader->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "read")).ToLocal(&read_fn_val) && read_fn_val->IsFunction()) {
                v8::Local<v8::Value> promise_val;
                if (read_fn_val.As<v8::Function>()->Call(context, reader, 0, nullptr).ToLocal(&promise_val) && promise_val->IsPromise()) {
                    v8::Local<v8::Promise> promise = promise_val.As<v8::Promise>();
                    
                    auto on_fulfilled = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                        v8::Isolate* p_isolate = args.GetIsolate();
                        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                        v8::Local<v8::Object> self = args.Data().As<v8::Object>();
                        v8::Local<v8::Object> result = args[0].As<v8::Object>();
                        
                        v8::Local<v8::Value> done_val;
                        if (result->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "done")).ToLocal(&done_val) && done_val->BooleanValue(p_isolate)) {
                            v8::Local<v8::Value> push_fn;
                            if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                                v8::Local<v8::Value> push_argv[] = { v8::Null(p_isolate) };
                                (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
                            }
                        } else {
                            v8::Local<v8::Value> value;
                            if (result->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "value")).ToLocal(&value)) {
                                v8::Local<v8::Value> push_fn;
                                if (self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) && push_fn->IsFunction()) {
                                    v8::Local<v8::Value> push_argv[] = { value };
                                    (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
                                }
                            }
                        }
                    };
                    
                    (void)promise->Then(context, v8::Function::New(context, on_fulfilled, self).ToLocalChecked());
                }
            }
        }
    };
    
    (void)node_readable->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_read"), v8::Function::New(context, read_fn).ToLocalChecked());
    args.GetReturnValue().Set(node_readable);
}

void Stream::readableToWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Object> options = v8::Object::New(p_isolate);
    (void)options->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying"), self);
    
    v8::Local<v8::Function> web_rs_ctor = createWebReadableStreamTemplate(p_isolate)->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Value> web_rs_argv[] = { options };
    v8::Local<v8::Object> web_rs;
    if (web_rs_ctor->NewInstance(context, 1, web_rs_argv).ToLocal(&web_rs)) {
        args.GetReturnValue().Set(web_rs);
    }
}

void Stream::writableFromWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() == 0 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "WritableStream required")));
        return;
    }
    
    v8::Local<v8::Object> web_stream = args[0].As<v8::Object>();
    
    // Create Node Writable
    v8::Local<v8::FunctionTemplate> node_writable_tmpl = getWritableTemplate(p_isolate);
    v8::Local<v8::Object> node_writable = node_writable_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    
    (void)node_writable->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_webStream"), web_stream);
    
    auto write_fn = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* p_isolate = args.GetIsolate();
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> self = args.This();
        v8::Local<v8::Value> chunk = args[0];
        v8::Local<v8::Value> callback = args[2]; // Node _write(chunk, encoding, callback)
        
        v8::Local<v8::Value> web_stream_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_webStream")).ToLocal(&web_stream_val) || !web_stream_val->IsObject()) return;
        v8::Local<v8::Object> web_stream = web_stream_val.As<v8::Object>();
        
        v8::Local<v8::Value> writer_val;
        if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "_writer")).ToLocal(&writer_val) || !writer_val->IsObject()) {
            v8::Local<v8::Value> get_writer_fn;
            if (web_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "getWriter")).ToLocal(&get_writer_fn) && get_writer_fn->IsFunction()) {
                if (get_writer_fn.As<v8::Function>()->Call(context, web_stream, 0, nullptr).ToLocal(&writer_val)) {
                    (void)self->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_writer"), writer_val);
                }
            }
        }
        
        if (!writer_val.IsEmpty() && writer_val->IsObject()) {
            v8::Local<v8::Object> writer = writer_val.As<v8::Object>();
            v8::Local<v8::Value> write_method_val;
            if (writer->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "write")).ToLocal(&write_method_val) && write_method_val->IsFunction()) {
                v8::Local<v8::Value> write_argv[] = { chunk };
                v8::Local<v8::Value> promise_val;
                if (write_method_val.As<v8::Function>()->Call(context, writer, 1, write_argv).ToLocal(&promise_val) && promise_val->IsPromise()) {
                    v8::Local<v8::Promise> promise = promise_val.As<v8::Promise>();
                    
                    v8::Local<v8::Array> cb_data = v8::Array::New(p_isolate, 1);
                    cb_data->Set(context, 0, callback).Check();

                    auto on_fulfilled = [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                        v8::Isolate* p_isolate = args.GetIsolate();
                        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
                        v8::Local<v8::Function> cb = args.Data().As<v8::Array>()->Get(context, 0).ToLocalChecked().As<v8::Function>();
                        (void)cb->Call(context, v8::Undefined(p_isolate), 0, nullptr);
                    };
                    
                    (void)promise->Then(context, v8::Function::New(context, on_fulfilled, cb_data).ToLocalChecked());
                }
            }
        }
    };
    
    (void)node_writable->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_write"), v8::Function::New(context, write_fn).ToLocalChecked());
    args.GetReturnValue().Set(node_writable);
}

void Stream::writableToWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Object> options = v8::Object::New(p_isolate);
    (void)options->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_underlying"), self);
    
    v8::Local<v8::Function> web_ws_ctor = createWebWritableStreamTemplate(p_isolate)->GetFunction(context).ToLocalChecked();
    v8::Local<v8::Value> web_ws_argv[] = { options };
    v8::Local<v8::Object> web_ws;
    if (web_ws_ctor->NewInstance(context, 1, web_ws_argv).ToLocal(&web_ws)) {
        args.GetReturnValue().Set(web_ws);
    }
}

void Stream::duplexFromWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    
    if (args.Length() == 0 || !args[0]->IsObject()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "WebStream required")));
        return;
    }
    
    v8::Local<v8::Object> web_stream = args[0].As<v8::Object>();
    
    // Create Node Duplex
    v8::Local<v8::FunctionTemplate> node_duplex_tmpl = getDuplexTemplate(p_isolate);
    v8::Local<v8::Object> node_duplex = node_duplex_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    
    // For Duplex.fromWeb, we expect an object with { readable, writable }
    v8::Local<v8::Value> readable_val, writable_val;
    if (web_stream->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "readable")).ToLocal(&readable_val) && readable_val->IsObject()) {
        // ...
    }

    // Node.js Duplex.fromWeb is often used for TransformStreams or similar
    // For now, return a placeholder that combines Readable and Writable logic
    p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Duplex.fromWeb not fully implemented")));
}

void Stream::duplexToWeb(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    
    v8::Local<v8::Object> result = v8::Object::New(p_isolate);
    
    // Call existing interop methods
    readableToWeb(args);
    v8::Local<v8::Value> readable_web = args.GetReturnValue().Get();
    (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "readable"), readable_web);
    
    writableToWeb(args);
    v8::Local<v8::Value> writable_web = args.GetReturnValue().Get();
    (void)result->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "writable"), writable_web);
    
    args.GetReturnValue().Set(result);
}

} // namespace module
} // namespace z8
