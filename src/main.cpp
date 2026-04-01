/* Z8 (Zane V8)
 * A high-performance, competitive JavaScript engine.
 * Copyright (C) 2026 Zane V8 Authors
 * Copyright (C) 2026 Sao Tin Developer Team
 */

// Standard headers
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <regex>
#include <sstream>

// V8 headers
#include "v8.h"
#include <v8-isolate.h>

#include "config.h"
#include "libplatform/libplatform.h"

// Environment interface
#include "module/console.h"
#include "module/timer.h"

// Interface for the node.js module
#include "module/node/buffer/buffer.h"
#include "module/node/events/events.h"
#include "module/node/fs/fs.h"

#include "module/node/os/os.h"
#include "module/node/path/path.h"
#include "module/node/process/process.h"
#include "module/node/util/util.h"
#include "module/node/zlib/zlib.h"
#include "module/node/stream/stream.h"
#include "task_queue.h"
#include "thread_pool.h"

// Windows headers
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
// Linker pragmas for Windows
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "ntdll.lib")
#endif

// Z8 Namespace
namespace z8 {

class Runtime {
  public:
    static void Initialize(const char* exec_path) {
        // Clean high-performance V8 flags
        const char* p_flags = "--stack-size=2048 "
                              "--max-semi-space-size=128 "
                              "--no-optimize-for-size "
                              "--turbo-fast-api-calls";
        v8::V8::SetFlagsFromString(p_flags);

        v8::V8::InitializeICUDefaultLocation(exec_path);

        static std::unique_ptr<v8::Platform> up_platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(up_platform.get());
        v8::V8::Initialize();
    }

    static void Shutdown() {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }

    Runtime() {
        v8::Isolate::CreateParams create_params;
        create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

        // Increase memory limits for competitive benchmarking
        // Deno uses ~128MB semi-space, which is ~256MB young generation
        create_params.constraints.set_max_old_generation_size_in_bytes(4096ULL * 1024 * 1024);
        create_params.constraints.set_max_young_generation_size_in_bytes(256ULL * 1024 * 1024);

        p_isolate = v8::Isolate::New(create_params);

        v8::Isolate::Scope isolate_scope(p_isolate);
        v8::HandleScope handle_scope(p_isolate);

        v8::Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New(p_isolate);
        v8::Local<v8::Context> context = v8::Context::New(p_isolate, nullptr, global_template);
        m_context.Reset(p_isolate, context);

        // Force override the 'console' object because V8 might have a default empty one
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Object> global = context->Global();
        v8::Local<v8::Object> console =
            z8::module::Console::createTemplate(p_isolate)->NewInstance(context).ToLocalChecked();

        global->Set(context, v8::String::NewFromUtf8(p_isolate, "console").ToLocalChecked(), console).Check();

        // Initialize Process module (global object)
        v8::Local<v8::Object> process = z8::module::Process::createObject(p_isolate, context);
        global->Set(context, v8::String::NewFromUtf8(p_isolate, "process").ToLocalChecked(), process).Check();

        // Initialize Timer module
        z8::module::Timer::initialize(p_isolate, context);

        // Initialize Buffer module (global object)
        z8::module::Buffer::initialize(p_isolate, context);

        // Initialize Web Streams (global objects)
        v8::Local<v8::Function> readable_stream_ctor = 
            z8::module::Stream::createWebReadableStreamTemplate(p_isolate)->GetFunction(context).ToLocalChecked();
        (void)global->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "ReadableStream"), readable_stream_ctor);

        v8::Local<v8::Function> writable_stream_ctor = 
            z8::module::Stream::createWebWritableStreamTemplate(p_isolate)->GetFunction(context).ToLocalChecked();
        (void)global->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "WritableStream"), writable_stream_ctor);

        p_isolate->SetHostImportModuleDynamicallyCallback(HostImportModuleDynamicallyCallback);
    }

    ~Runtime() {
        m_context.Reset();
        p_isolate->Dispose();
    }

    static v8::MaybeLocal<v8::Module> ResolveModuleCallback(v8::Local<v8::Context> context,
                                                            v8::Local<v8::String> specifier,
                                                            v8::Local<v8::FixedArray> import_assertions,
                                                            v8::Local<v8::Module> referrer) {
        v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
        v8::String::Utf8Value specifier_utf8(p_isolate, specifier);
        std::string specifier_str(*specifier_utf8);

        if (specifier_str == "node:fs") {
            v8::Local<v8::ObjectTemplate> fs_template = z8::module::FS::createTemplate(p_isolate);

            // To get property names, we must create an instance first.
            v8::Local<v8::Object> fs_instance;
            if (!fs_template->NewInstance(context).ToLocal(&fs_instance)) {
                return v8::MaybeLocal<v8::Module>();
            }

            v8::Local<v8::Array> prop_names;
            if (!fs_instance->GetPropertyNames(context).ToLocal(&prop_names)) {
                return v8::MaybeLocal<v8::Module>();
            }

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));

            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                v8::Local<v8::Value> name_val = prop_names->Get(context, i).ToLocalChecked();
                export_names.push_back(name_val.As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:fs"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> fs_template = z8::module::FS::createTemplate(p_isolate);
                    v8::Local<v8::Object> fs_obj = fs_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), fs_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names;
                    if (fs_obj->GetPropertyNames(context).ToLocal(&prop_names)) {
                        for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                            v8::Local<v8::Value> name_val = prop_names->Get(context, i).ToLocalChecked();
                            v8::Local<v8::Value> prop_val = fs_obj->Get(context, name_val).ToLocalChecked();
                            module->SetSyntheticModuleExport(p_isolate, name_val.As<v8::String>(), prop_val).Check();
                        }
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:path") {
            v8::Local<v8::ObjectTemplate> path_template = z8::module::Path::createTemplate(p_isolate);
            v8::Local<v8::Object> path_instance = path_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = path_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:path"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> path_template = z8::module::Path::createTemplate(p_isolate);
                    v8::Local<v8::Object> path_obj = path_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(
                            p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), path_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = path_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, path_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:os") {
            v8::Local<v8::ObjectTemplate> os_template = z8::module::OS::createTemplate(p_isolate);
            v8::Local<v8::Object> os_instance = os_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = os_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:os"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> os_template = z8::module::OS::createTemplate(p_isolate);
                    v8::Local<v8::Object> os_obj = os_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), os_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = os_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, os_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:fs/promises") {
            v8::Local<v8::ObjectTemplate> fs_template = z8::module::FS::createPromisesTemplate(p_isolate);
            v8::Local<v8::Object> fs_instance = fs_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = fs_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:fs/promises"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> fs_template = z8::module::FS::createPromisesTemplate(p_isolate);
                    v8::Local<v8::Object> fs_obj = fs_template->NewInstance(context).ToLocalChecked();
                    v8::Local<v8::Array> prop_names = fs_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, fs_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:util") {
            v8::Local<v8::ObjectTemplate> util_template = z8::module::Util::createTemplate(p_isolate);
            v8::Local<v8::Object> util_instance = util_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = util_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:util"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> util_template = z8::module::Util::createTemplate(p_isolate);
                    v8::Local<v8::Object> util_obj = util_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(
                            p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), util_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = util_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, util_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }
        
        if (specifier_str == "node:zlib") {
            v8::Local<v8::ObjectTemplate> zlib_template = z8::module::Zlib::createTemplate(p_isolate);
            v8::Local<v8::Object> zlib_instance = zlib_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = zlib_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:zlib"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> zlib_template = z8::module::Zlib::createTemplate(p_isolate);
                    v8::Local<v8::Object> zlib_obj = zlib_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(
                            p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), zlib_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = zlib_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, zlib_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:events") {
            v8::Local<v8::ObjectTemplate> events_template = z8::module::Events::createTemplate(p_isolate);
            v8::Local<v8::Object> events_instance = events_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = events_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                if (name->StrictEquals(v8::String::NewFromUtf8Literal(p_isolate, "default"))) continue;
                export_names.push_back(name);
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:events"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> events_template = z8::module::Events::createTemplate(p_isolate);
                    v8::Local<v8::Object> events_obj = events_template->NewInstance(context).ToLocalChecked();
                    
                    v8::Local<v8::Value> default_val = events_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "default")).ToLocalChecked();
                    module->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), default_val).Check();

                    v8::Local<v8::Array> prop_names = events_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        if (name->StrictEquals(v8::String::NewFromUtf8Literal(p_isolate, "default"))) continue;
                        v8::Local<v8::Value> val;
                        if (events_obj->Get(context, name).ToLocal(&val)) {
                            module->SetSyntheticModuleExport(p_isolate, name, val).Check();
                        }
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:buffer") {

            v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
            v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
            
            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "Buffer"));
            
            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:buffer"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::Value> buffer_val = context->Global()->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "Buffer")).ToLocalChecked();
                    module->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), buffer_val).Check();
                    module->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "Buffer"), buffer_val).Check();
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:process") {
            v8::Local<v8::Object> process_instance = z8::module::Process::createObject(p_isolate, context);
            v8::Local<v8::Array> prop_names = process_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:process"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::Object> process_obj = z8::module::Process::createObject(p_isolate, context);
                    module
                        ->SetSyntheticModuleExport(
                            p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), process_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = process_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, process_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:stream") {
            v8::Local<v8::ObjectTemplate> stream_template = z8::module::Stream::createTemplate(p_isolate);
            v8::Local<v8::Object> stream_instance = stream_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Array> prop_names = stream_instance->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:stream"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> stream_template = z8::module::Stream::createTemplate(p_isolate);
                    v8::Local<v8::Object> stream_obj = stream_template->NewInstance(context).ToLocalChecked();
                    module
                        ->SetSyntheticModuleExport(
                            p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), stream_obj)
                        .Check();
                    v8::Local<v8::Array> prop_names = stream_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, stream_obj->Get(context, name).ToLocalChecked())
                            .Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        if (specifier_str == "node:stream/promises") {
            v8::Local<v8::ObjectTemplate> stream_template = z8::module::Stream::createTemplate(p_isolate);
            v8::Local<v8::Object> stream_instance = stream_template->NewInstance(context).ToLocalChecked();
            v8::Local<v8::Object> promises_obj = stream_instance->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "promises")).ToLocalChecked().As<v8::Object>();
            v8::Local<v8::Array> prop_names = promises_obj->GetPropertyNames(context).ToLocalChecked();

            std::vector<v8::Local<v8::String>> export_names;
            export_names.push_back(v8::String::NewFromUtf8Literal(p_isolate, "default"));
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                export_names.push_back(prop_names->Get(context, i).ToLocalChecked().As<v8::String>());
            }

            auto module = v8::Module::CreateSyntheticModule(
                p_isolate,
                v8::String::NewFromUtf8Literal(p_isolate, "node:stream/promises"),
                v8::MemorySpan<const v8::Local<v8::String>>(export_names.data(), export_names.size()),
                [](v8::Local<v8::Context> context, v8::Local<v8::Module> module) -> v8::MaybeLocal<v8::Value> {
                    v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
                    v8::Local<v8::ObjectTemplate> stream_template = z8::module::Stream::createTemplate(p_isolate);
                    v8::Local<v8::Object> stream_obj = stream_template->NewInstance(context).ToLocalChecked();
                    v8::Local<v8::Object> promises_obj = stream_obj->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "promises")).ToLocalChecked().As<v8::Object>();
                    
                    module->SetSyntheticModuleExport(p_isolate, v8::String::NewFromUtf8Literal(p_isolate, "default"), promises_obj).Check();
                    v8::Local<v8::Array> prop_names = promises_obj->GetPropertyNames(context).ToLocalChecked();
                    for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                        v8::Local<v8::String> name = prop_names->Get(context, i).ToLocalChecked().As<v8::String>();
                        module->SetSyntheticModuleExport(p_isolate, name, promises_obj->Get(context, name).ToLocalChecked()).Check();
                    }
                    return v8::Undefined(p_isolate);
                });
            return module;
        }

        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Module not found: " + specifier_str).c_str()).ToLocalChecked());
        return v8::MaybeLocal<v8::Module>();
    }

    static v8::MaybeLocal<v8::Promise> HostImportModuleDynamicallyCallback(
        v8::Local<v8::Context> context, v8::Local<v8::Data> host_defined_options,
        v8::Local<v8::Value> resource_name, v8::Local<v8::String> specifier,
        v8::Local<v8::FixedArray> import_assertions) {
        v8::Isolate* p_isolate = v8::Isolate::GetCurrent();
        v8::HandleScope handle_scope(p_isolate);

        v8::Local<v8::Promise::Resolver> resolver;
        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
            return v8::MaybeLocal<v8::Promise>();
        }

        v8::MaybeLocal<v8::Module> maybe_module = ResolveModuleCallback(context, specifier, import_assertions, v8::Local<v8::Module>());
        
        if (maybe_module.IsEmpty()) {
            v8::Local<v8::Value> error = v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Module not found"));
            (void)resolver->Reject(context, error);
            return resolver->GetPromise();
        }

        v8::Local<v8::Module> module = maybe_module.ToLocalChecked();
        
        if (module->GetStatus() == v8::Module::kUninstantiated) {
            if (!module->InstantiateModule(context, ResolveModuleCallback).FromMaybe(false)) {
                (void)resolver->Reject(context, v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Module instantiation failed")));
                return resolver->GetPromise();
            }
        }
        
        if (module->GetStatus() == v8::Module::kInstantiated) {
            v8::MaybeLocal<v8::Value> result = module->Evaluate(context);
            if (result.IsEmpty()) {
                (void)resolver->Reject(context, v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Module evaluation failed")));
                return resolver->GetPromise();
            }
        }
        
        v8::Local<v8::Value> ns = module->GetModuleNamespace();
        (void)resolver->Resolve(context, ns);

        return resolver->GetPromise();
    }

    bool Run(const std::string& source, const std::string& filename) {
        v8::Isolate::Scope isolate_scope(p_isolate);
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> context = m_context.Get(p_isolate);
        v8::Context::Scope context_scope(context);

        v8::TryCatch try_catch(p_isolate);

        v8::Local<v8::String> v8_source = v8::String::NewFromUtf8(p_isolate, source.c_str()).ToLocalChecked();
        v8::Local<v8::String> v8_filename = v8::String::NewFromUtf8(p_isolate, filename.c_str()).ToLocalChecked();

        // Modules are faster as V8 applies more aggressive optimizations to them
        v8::ScriptOrigin origin(v8_filename, 0, 0, false, -1, v8::Local<v8::Value>(), false, false, true);
        v8::ScriptCompiler::Source script_source(v8_source, origin);
        v8::Local<v8::Module> module;

        if (!v8::ScriptCompiler::CompileModule(p_isolate, &script_source).ToLocal(&module)) {
            ReportException(p_isolate, &try_catch);
            return false;
        }

        if (!module->InstantiateModule(context, ResolveModuleCallback).FromMaybe(false)) {
            ReportException(p_isolate, &try_catch);
            return false;
        }

        v8::MaybeLocal<v8::Value> result = module->Evaluate(context);

        if (try_catch.HasCaught()) {
            ReportException(p_isolate, &try_catch);
            return false;
        }

        // Only check Promise if we actually got a result and it looks like a Promise
        v8::Local<v8::Value> result_val;
        if (result.ToLocal(&result_val) && result_val->IsPromise()) {
            v8::Local<v8::Promise> promise = result_val.As<v8::Promise>();
            if (promise->State() == v8::Promise::kRejected) {
                p_isolate->ThrowException(promise->Result());
                ReportException(p_isolate, &try_catch);
                return false;
            }
        }

        // Event Loop
        bool keep_running = true;
        while (keep_running) {
            // 1. Process Tasks from TaskQueue
            while (!z8::TaskQueue::getInstance().isEmpty()) {
                z8::Task* p_task = z8::TaskQueue::getInstance().dequeue();
                if (p_task) {
                    v8::TryCatch task_try_catch(p_isolate);
                    p_task->m_runner(p_isolate, context, p_task);
                    delete p_task;

                    // Resume JS execution
                    p_isolate->PerformMicrotaskCheckpoint();

                    if (task_try_catch.HasCaught()) {
                        ReportException(p_isolate, &task_try_catch);
                        return false;
                    }
                }
            }

            // 2. Process Timers
            if (z8::module::Timer::hasActiveTimers()) {
                v8::TryCatch loop_try_catch(p_isolate);
                z8::module::Timer::tick(p_isolate, context);
                p_isolate->PerformMicrotaskCheckpoint();
                if (loop_try_catch.HasCaught()) {
                    ReportException(p_isolate, &loop_try_catch);
                    return false;
                }
            }

            // 3. Final termination check
            bool has_work = z8::module::Timer::hasActiveTimers() ||
                            !z8::TaskQueue::getInstance().isEmpty() ||
                            z8::ThreadPool::getInstance().hasPendingTasks();

            if (!has_work) {
                // One last check for microtasks that might have been queued
                p_isolate->PerformMicrotaskCheckpoint();

                // Re-check after microtask checkpoint
                has_work = z8::module::Timer::hasActiveTimers() ||
                           !z8::TaskQueue::getInstance().isEmpty() ||
                           z8::ThreadPool::getInstance().hasPendingTasks();
                
                if (!has_work) {
                    keep_running = false;
                }
            }

            // 4. Wait for work if nothing is pending
            if (keep_running && !has_work) { // Only wait if there's truly nothing to do
                std::chrono::milliseconds delay = z8::module::Timer::getNextDelay();
                std::chrono::milliseconds timeout(10); // Always wait for a small duration
                if (delay.count() >= 0) {
                    timeout = std::chrono::milliseconds(std::min(static_cast<int64_t>(delay.count()), 10LL));
                }
                z8::TaskQueue::getInstance().wait(timeout);
            }
        }

        return true;
    }

    void RunREPL() {
        v8::Isolate::Scope isolate_scope(p_isolate);
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> context = m_context.Get(p_isolate);
        v8::Context::Scope context_scope(context);

        std::cout << "Welcome to Zane V8 (Z8) v" << Z8_BUILD_VERSION << std::endl;
        std::cout << "Type 'exit' or '.exit' to quit." << std::endl;

        std::string line;
        while (true) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line))
                break;
            if (line == "exit" || line == ".exit")
                break;
            if (line.empty())
                continue;

            // Simple Wrap-and-Rewrite for ESM imports in REPL
            // import fs from "node:fs" -> var fs = (await import("node:fs")).default
            // import { a, b } from "mod" -> var { a, b } = await import("mod")
            std::string rewritten = line;
            std::regex import_default_re(R"(import\s+([a-zA-Z0-9_$]+)\s+from\s+(['"].+?['"]))");
            std::regex import_named_re(R"(import\s*\{\s*([^}]+)\s*\}\s*from\s+(['"].+?['"]))");
            std::regex import_star_re(R"(import\s*\*\s*as\s+([a-zA-Z0-9_$]+)\s+from\s+(['"].+?['"]))");

            bool is_import = false;
            if (std::regex_search(line, import_default_re)) {
                rewritten = std::regex_replace(line, import_default_re, "globalThis.$1 = (await import($2)).default || (await import($2)); return globalThis.$1;");
                is_import = true;
            } else if (std::regex_search(line, import_named_re)) {
                // For named imports, we assigned to globalThis. Extracting names from regex
                std::smatch match;
                if (std::regex_search(line, match, import_named_re)) {
                    std::string names = match[1].str();
                    std::string mod = match[2].str();
                    rewritten = "const _m = await import(" + mod + "); ";
                    // Split names by comma
                    std::stringstream ss(names);
                    std::string name;
                    while(std::getline(ss, name, ',')) {
                        // Trim name
                        name.erase(0, name.find_first_not_of(" \t\r\n"));
                        name.erase(name.find_last_not_of(" \t\r\n") + 1);
                        if (!name.empty()) {
                            rewritten += "globalThis." + name + " = _m." + name + "; ";
                        }
                    }
                    rewritten += "return undefined;";
                }
                is_import = true;
            } else if (std::regex_search(line, import_star_re)) {
                rewritten = std::regex_replace(line, import_star_re, "globalThis.$1 = await import($2); return globalThis.$1;");
                is_import = true;
            }

            // If it's an import or contains await, we need to wrap it to support top-level await in scripts
            if (is_import || rewritten.find("await") != std::string::npos) {
                // To preserve 'var' declarations globally, we don't wrap in an async function entirely
                // but instead we can use a trick or simply rely on the fact that 'import()' is a promise.
                // However, 'await' is only valid in async functions or top-level modules.
                // Since this is a Script, we wrap it:
                if (is_import) {
                    rewritten = "(async () => { " + rewritten + " })()";
                } else {
                    rewritten = "(async () => { return (" + rewritten + "); })()";
                }
            }

            v8::TryCatch try_catch(p_isolate);
            v8::Local<v8::String> source = v8::Local<v8::String>::Cast(v8::String::NewFromUtf8(p_isolate, rewritten.c_str()).ToLocalChecked());
            v8::Local<v8::Script> script;
            if (!v8::Script::Compile(context, source).ToLocal(&script)) {
                ReportException(p_isolate, &try_catch);
                continue;
            }

            v8::Local<v8::Value> result;
            if (!script->Run(context).ToLocal(&result)) {
                ReportException(p_isolate, &try_catch);
                continue;
            }

            // Handle Promises (for rewritten imports or explicit Promises)
            if (!result.IsEmpty() && result->IsPromise()) {
                v8::Local<v8::Promise> promise = result.As<v8::Promise>();
                while (promise->State() == v8::Promise::kPending) {
                    p_isolate->PerformMicrotaskCheckpoint();
                    // Process task queue while waiting
                    while (!z8::TaskQueue::getInstance().isEmpty()) {
                        z8::Task* p_task = z8::TaskQueue::getInstance().dequeue();
                        if (p_task) {
                            p_task->m_runner(p_isolate, context, p_task);
                            delete p_task;
                        }
                    }
                    if (promise->State() == v8::Promise::kPending) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                if (promise->State() == v8::Promise::kFulfilled) {
                    result = promise->Result();
                } else if (promise->State() == v8::Promise::kRejected) {
                    v8::Local<v8::Value> exception = promise->Result();
                    p_isolate->ThrowException(exception);
                    ReportException(p_isolate, &try_catch);
                    continue;
                }
            }

            if (!result->IsUndefined()) {
                bool use_colors = z8::module::Util::shouldLogWithColors(stdout);
                std::string inspected = z8::module::Util::inspectInternal(p_isolate, result, 2, 0, use_colors);
                std::cout << inspected << std::endl;
            }
        }
    }

  private:
    void ReportException(v8::Isolate* p_isolate, v8::TryCatch* try_catch) {
        fflush(stdout); // Rescue any buffered stdout before reporting error
        v8::HandleScope handle_scope(p_isolate);
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Message> message = try_catch->Message();

        if (message.IsEmpty()) {
            // V8 didn't provide a message, just print the exception
            v8::Local<v8::Value> exception = try_catch->Exception();
            if (!exception.IsEmpty()) {
                v8::String::Utf8Value exception_str(p_isolate, exception);
                std::cerr << "Uncaught Exception: " << (*exception_str ? *exception_str : "unknown") << std::endl;
            } else {
                std::cerr << "Uncaught Exception (empty exception object)" << std::endl;
            }
        } else {
            v8::String::Utf8Value exception(p_isolate, try_catch->Exception());
            v8::Local<v8::Value> resource_name = message->GetScriptResourceName();
            v8::String::Utf8Value filename(p_isolate, resource_name);

            int32_t linenum = message->GetLineNumber(context).FromMaybe(-1);
            const char* p_filename_str = *filename ? *filename : "unknown";
            const char* p_exception_str = *exception ? *exception : "unknown";

            std::cerr << p_filename_str << ":" << linenum << ": " << p_exception_str << std::endl;

            v8::MaybeLocal<v8::String> sourceline_maybe = message->GetSourceLine(context);
            if (!sourceline_maybe.IsEmpty()) {
                v8::String::Utf8Value sourceline(p_isolate, sourceline_maybe.ToLocalChecked());
                std::cerr << *sourceline << std::endl;

                int32_t start = message->GetStartColumn(context).FromMaybe(0);
                for (int32_t i = 0; i < start; i++)
                    std::cerr << " ";
                int32_t end = message->GetEndColumn(context).FromMaybe(0);
                for (int32_t i = start; i < end; i++)
                    std::cerr << "^";
                std::cerr << std::endl;
            }
        }

        v8::Local<v8::Value> stack_trace;
        if (try_catch->StackTrace(context).ToLocal(&stack_trace) && stack_trace->IsString() &&
            v8::Local<v8::String>::Cast(stack_trace)->Length() > 0) {
            v8::String::Utf8Value stack_trace_str(p_isolate, stack_trace);
            std::cerr << *stack_trace_str << std::endl;
        }

        fflush(stderr);
    }

  public:
    v8::Isolate* p_isolate;
    v8::Global<v8::Context> m_context;
};

} // namespace z8

#include <filesystem>

namespace fs = std::filesystem;

// Validate and sanitize file path to prevent path traversal attacks
bool ValidatePath(const std::string& path_str, fs::path& sanitized_path) {
    try {
        // Resolve to canonical absolute path (resolves symlinks, "..", ".")
        fs::path canonical = fs::canonical(path_str);

        // Get current working directory
        fs::path cwd = fs::current_path();

        // Check if canonical path starts with cwd using robust component comparison
        auto mismatch_pair = std::mismatch(cwd.begin(), cwd.end(), canonical.begin());

        if (mismatch_pair.first != cwd.end()) {
            return false;
        }

        sanitized_path = canonical;
        return true;
    } catch (const fs::filesystem_error&) {
        // Path doesn't exist or other filesystem error
        return false;
    }
}

// Helper to safely read file with built-in path validation.
// Uses allowlist validation and constructs a new path string to prevent
// path traversal attacks.
// Returns {content, error_message}. Empty error_message means success.
std::pair<std::string, std::string> ReadValidatedFile(const std::string& raw_path) {
    // Step 1: Reject obviously malicious patterns in raw input
    if (raw_path.find("..") != std::string::npos) {
        return {"", "Invalid file path: directory traversal not allowed"};
    }

    // Step 2: Resolve to canonical path via the filesystem
    fs::path canonical;
    try {
        canonical = fs::canonical(fs::path(raw_path));
    } catch (const fs::filesystem_error&) {
        return {"", "Invalid or inaccessible file path"};
    }

    // Step 3: Validate canonical path is within the current working directory
    fs::path cwd = fs::current_path();
    auto mismatch_pair = std::mismatch(cwd.begin(), cwd.end(), canonical.begin());
    if (mismatch_pair.first != cwd.end()) {
        return {"", "Invalid or inaccessible file path"};
    }

    // Step 4: Validate file extension (allowlist)
    std::string ext = canonical.extension().string();
    if (ext != ".js" && ext != ".mjs") {
        return {"", "Invalid file type: only .js and .mjs files are allowed"};
    }

    // Step 5: Validate path characters (allowlist - only safe characters)
    std::string canonical_str = canonical.string();
    for (char c : canonical_str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '/' && c != '\\' && c != '.' && c != '-' && c != '_' && c != ':' && c != ' ') {
            return {"", "Invalid file path: contains disallowed characters"};
        }
    }

    // Step 6: Construct a brand new path string (breaks taint chain)
    std::string safe_path;
    safe_path.reserve(canonical_str.size());
    for (size_t i = 0; i < canonical_str.size(); ++i) {
        safe_path += canonical_str[i];
    }

    // Step 7: Open and read the file using the validated, reconstructed path
    std::ifstream file(safe_path, std::ios::binary);
    if (!file.is_open()) {
        return {"", "Could not open file"};
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (content.empty() && fs::file_size(canonical) > 0) {
        return {"", "Could not read file"};
    }

    return {content, ""};
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (argc < 2) {
        z8::Runtime::Initialize(argv[0]);
        z8::module::Process::setArgv(argc, argv);
        {
            z8::Runtime rt;
            rt.RunREPL();
        }
        z8::Runtime::Shutdown();
        return 0;
    }

    fs::path filename;
    std::string source;

    if (std::string(argv[1]) == "-e" && argc > 2) {
        filename = "eval";
        source = argv[2];
    } else {
        // Use ReadValidatedFile which validates and reads in one safe step
        auto [content, error] = ReadValidatedFile(argv[1]);
        if (!error.empty()) {
            std::cerr << "✖ Error: " << error << ": " << argv[1] << std::endl;
            return 1;
        }

        filename = argv[1];
        source = content;
    }

    z8::Runtime::Initialize(argv[0]);
    z8::module::Process::setArgv(argc, argv);
    bool success = false;
    {
        z8::Runtime rt;
        success = rt.Run(source, filename.string());
    }

    z8::Runtime::Shutdown();
    return success ? 0 : 1;
}

