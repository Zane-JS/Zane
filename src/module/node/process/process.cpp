#include "process.hpp"
#include "config.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <psapi.h>
#else
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "../../../../deps/zlib/zlib.h"
#include "../../../../deps/brotli/c/include/brotli/decode.h"
#include "../../../../deps/brotli/c/include/brotli/encode.h"
#include "../../../../deps/zstd/lib/zstd.h"

namespace z8 {
namespace module {

namespace fs = std::filesystem;

static auto start_time = std::chrono::steady_clock::now();
std::vector<std::string> Process::m_argv;

void Process::setArgv(int32_t argc, char* argv[]) {
    m_argv.clear();
    for (int32_t i = 0; i < argc; ++i) {
        m_argv.push_back(argv[i]);
    }
}

v8::Local<v8::ObjectTemplate> Process::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "cwd"), v8::FunctionTemplate::New(p_isolate, cwd));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "chdir"), v8::FunctionTemplate::New(p_isolate, chdir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "exit"), v8::FunctionTemplate::New(p_isolate, exit));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "uptime"), v8::FunctionTemplate::New(p_isolate, uptime));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "nextTick"), v8::FunctionTemplate::New(p_isolate, nextTick));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "memoryUsage"), v8::FunctionTemplate::New(p_isolate, memoryUsage));
    
    v8::Local<v8::FunctionTemplate> hrtime_tmpl = v8::FunctionTemplate::New(p_isolate, hrtime);
    hrtime_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "bigint"), v8::FunctionTemplate::New(p_isolate, hrtimeBigInt));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "hrtime"), hrtime_tmpl);

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "kill"), v8::FunctionTemplate::New(p_isolate, kill));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "umask"), v8::FunctionTemplate::New(p_isolate, umask));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "cpuUsage"), v8::FunctionTemplate::New(p_isolate, cpuUsage));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "resourceUsage"), v8::FunctionTemplate::New(p_isolate, resourceUsage));
    
    // Register Event Emitter stubs
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "on"), v8::FunctionTemplate::New(p_isolate, on));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "once"), v8::FunctionTemplate::New(p_isolate, once));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "addListener"), v8::FunctionTemplate::New(p_isolate, on));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "off"), v8::FunctionTemplate::New(p_isolate, off));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "removeListener"), v8::FunctionTemplate::New(p_isolate, off));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "emit"), v8::FunctionTemplate::New(p_isolate, emit));

    return tmpl;
}

v8::Local<v8::Object> Process::createObject(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::Local<v8::ObjectTemplate> tmpl = createTemplate(p_isolate);
    v8::Local<v8::Object> obj = tmpl->NewInstance(context).ToLocalChecked();

    // Set process.title accessor
    obj->SetNativeDataProperty(context, 
                               v8::String::NewFromUtf8Literal(p_isolate, "title"), 
                               getTitle, 
                               setTitle).Check();

    // Set process.env
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "env"), createEnvObject(p_isolate, context)).Check();

    // Set process.argv
    v8::Local<v8::Array> argv_arr = v8::Array::New(p_isolate, static_cast<int32_t>(m_argv.size()));
    for (size_t i = 0; i < m_argv.size(); ++i) {
        argv_arr->Set(context, (uint32_t)i, v8::String::NewFromUtf8(p_isolate, m_argv[i].c_str()).ToLocalChecked())
            .Check();
    }
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "argv"), argv_arr).Check();

    // Set process.argv0
    if (!m_argv.empty()) {
        obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "argv0"), v8::String::NewFromUtf8(p_isolate, m_argv[0].c_str()).ToLocalChecked())
            .Check();
    }

    // Set process.pid
#ifdef _WIN32
    uint32_t pid = GetCurrentProcessId();
#else
    uint32_t pid = getpid();
#endif
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "pid"), v8::Number::New(p_isolate, pid)).Check();
    
    // Set process.execArgv
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "execArgv"), v8::Array::New(p_isolate, 0)).Check();

    // Set process.stdout, stderr, stdin with isTTY and fd property
    bool stdout_is_tty = false;
#ifdef _WIN32
    stdout_is_tty = _isatty(_fileno(stdout));
#else
    stdout_is_tty = isatty(fileno(stdout));
#endif
    v8::Local<v8::Object> stdout_obj = v8::Object::New(p_isolate);
    stdout_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "isTTY"), v8::Boolean::New(p_isolate, stdout_is_tty)).Check();
    stdout_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "fd"), v8::Number::New(p_isolate, 1)).Check();
    stdout_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, stdoutWrite)->GetFunction(context).ToLocalChecked()).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "stdout"), stdout_obj).Check();

    bool stderr_is_tty = false;
#ifdef _WIN32
    stderr_is_tty = _isatty(_fileno(stderr));
#else
    stderr_is_tty = isatty(fileno(stderr));
#endif
    v8::Local<v8::Object> stderr_obj = v8::Object::New(p_isolate);
    stderr_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "isTTY"), v8::Boolean::New(p_isolate, stderr_is_tty)).Check();
    stderr_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "fd"), v8::Number::New(p_isolate, 2)).Check();
    stderr_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, stderrWrite)->GetFunction(context).ToLocalChecked()).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "stderr"), stderr_obj).Check();

    bool stdin_is_tty = false;
#ifdef _WIN32
    stdin_is_tty = _isatty(_fileno(stdin));
#else
    stdin_is_tty = isatty(fileno(stdin));
#endif
    v8::Local<v8::Object> stdin_obj = v8::Object::New(p_isolate);
    stdin_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "isTTY"), v8::Boolean::New(p_isolate, stdin_is_tty)).Check();
    stdin_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "fd"), v8::Number::New(p_isolate, 0)).Check();
    stdin_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "read"), v8::FunctionTemplate::New(p_isolate, stdinRead)->GetFunction(context).ToLocalChecked()).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "stdin"), stdin_obj).Check();

    // Set process.platform
#ifdef _WIN32
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "platform"), v8::String::NewFromUtf8Literal(p_isolate, "win32"))
        .Check();
#elif __APPLE__
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "platform"), v8::String::NewFromUtf8Literal(p_isolate, "darwin"))
        .Check();
#else
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "platform"), v8::String::NewFromUtf8Literal(p_isolate, "linux"))
        .Check();
#endif

    // Set process.arch
#if defined(_M_X64) || defined(__x86_64__)
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::String::NewFromUtf8Literal(p_isolate, "x64"))
        .Check();
#elif defined(_M_IX86) || defined(__i386__)
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::String::NewFromUtf8Literal(p_isolate, "ia32"))
        .Check();
#elif defined(_M_ARM64) || defined(__aarch64__)
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::String::NewFromUtf8Literal(p_isolate, "arm64"))
        .Check();
#elif defined(_M_ARM) || defined(__arm__)
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::String::NewFromUtf8Literal(p_isolate, "arm"))
        .Check();
#else
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::String::NewFromUtf8Literal(p_isolate, "unknown"))
        .Check();
#endif

    // Set process.execPath
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "execPath"), v8::String::NewFromUtf8(p_isolate, getExecPath().c_str()).ToLocalChecked())
        .Check();

    // Set process.version
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "version"), v8::String::NewFromUtf8Literal(p_isolate, "v" Z8_APP_VERSION))
        .Check();

    // Set process.versions
    v8::Local<v8::Object> versions_obj = v8::Object::New(p_isolate);
    versions_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "z8"), v8::String::NewFromUtf8Literal(p_isolate, Z8_APP_VERSION))
        .Check();
    versions_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "node"), v8::String::NewFromUtf8Literal(p_isolate, Z8_NODE_VERSION)) // Report compatible Node.js version
        .Check();
    versions_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "napi"), v8::String::NewFromUtf8Literal(p_isolate, Z8_NAPI_VERSION))
        .Check();
    versions_obj
        ->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "modules"), v8::String::NewFromUtf8Literal(p_isolate, Z8_MODULES_VERSION))
        .Check();
    versions_obj
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "v8"),
              v8::String::NewFromUtf8(p_isolate, v8::V8::GetVersion()).ToLocalChecked())
        .Check();
    
    versions_obj
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "zlib"),
              v8::String::NewFromUtf8Literal(p_isolate, ZLIB_VERSION))
        .Check();

    uint32_t brotli_ver = BrotliDecoderVersion();
    char brotli_ver_str[32];
    snprintf(brotli_ver_str, sizeof(brotli_ver_str), "%u.%u.%u", 
             brotli_ver >> 24, (brotli_ver >> 12) & 0xFFF, brotli_ver & 0xFFF);
    versions_obj
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "brotli"),
              v8::String::NewFromUtf8(p_isolate, brotli_ver_str).ToLocalChecked())
        .Check();

    versions_obj
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "zstd"),
              v8::String::NewFromUtf8Literal(p_isolate, ZSTD_VERSION_STRING))
        .Check();

    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "versions"), versions_obj).Check();

    // Set process.release
    v8::Local<v8::Object> release_obj = v8::Object::New(p_isolate);
    release_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "name"), v8::String::NewFromUtf8Literal(p_isolate, "node")).Check();
    obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "release"), release_obj).Check();

    return obj;
}

void Process::cwd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    try {
        std::string path = fs::current_path().string();
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, path.c_str()).ToLocalChecked());
    } catch (...) {
        args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "."));
    }
}

void Process::chdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Directory must be a string")));
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    try {
        fs::current_path(*path);
    } catch (const std::exception& e) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, e.what()).ToLocalChecked()));
    }
}

void Process::exit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    int32_t code = 0;
    if (args.Length() > 0 && args[0]->IsInt32()) {
        code = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    }
    std::exit(code);
}

void Process::uptime(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto now = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), (double) diff.count()));
}

void Process::nextTick(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() > 0 && args[0]->IsFunction()) {
        p_isolate->EnqueueMicrotask(args[0].As<v8::Function>());
    }
}

void Process::memoryUsage(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    v8::HeapStatistics heap_stats;
    p_isolate->GetHeapStatistics(&heap_stats);

    v8::Local<v8::Object> res = v8::Object::New(p_isolate);
    res->Set(context,
             v8::String::NewFromUtf8Literal(p_isolate, "rss"),
             v8::BigInt::NewFromUnsigned(p_isolate, heap_stats.heap_size_limit())) // Rough estimate for RSS
        .Check();
    res->Set(context,
             v8::String::NewFromUtf8Literal(p_isolate, "heapTotal"),
             v8::BigInt::NewFromUnsigned(p_isolate, heap_stats.total_heap_size()))
        .Check();
    res->Set(context,
             v8::String::NewFromUtf8Literal(p_isolate, "heapUsed"),
             v8::BigInt::NewFromUnsigned(p_isolate, heap_stats.used_heap_size()))
        .Check();
    res->Set(context,
             v8::String::NewFromUtf8Literal(p_isolate, "external"),
             v8::BigInt::NewFromUnsigned(p_isolate, heap_stats.external_memory()))
        .Check();

    args.GetReturnValue().Set(res);
}

void Process::hrtime(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    if (args.Length() > 0 && args[0]->IsArray()) {
        v8::Local<v8::Array> prev = args[0].As<v8::Array>();
        if (prev->Length() >= 2) {
            uint64_t prev_sec = prev->Get(context, 0).ToLocalChecked()->Uint32Value(context).FromMaybe(0);
            uint64_t prev_nanos = prev->Get(context, 1).ToLocalChecked()->Uint32Value(context).FromMaybe(0);
            uint64_t prev_total = (prev_sec * 1000000000ULL) + prev_nanos;

            uint64_t diff = nanos - prev_total;
            v8::Local<v8::Array> res = v8::Array::New(p_isolate, 2);
            res->Set(context, 0, v8::Integer::NewFromUnsigned(p_isolate, (uint32_t)(diff / 1000000000ULL))).Check();
            res->Set(context, 1, v8::Integer::NewFromUnsigned(p_isolate, (uint32_t)(diff % 1000000000ULL))).Check();
            args.GetReturnValue().Set(res);
            return;
        }
    }

    v8::Local<v8::Array> res = v8::Array::New(p_isolate, 2);
    res->Set(context, 0, v8::Integer::NewFromUnsigned(p_isolate, (uint32_t)(nanos / 1000000000ULL))).Check();
    res->Set(context, 1, v8::Integer::NewFromUnsigned(p_isolate, (uint32_t)(nanos % 1000000000ULL))).Check();
    args.GetReturnValue().Set(res);
}

void Process::hrtimeBigInt(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto now = std::chrono::high_resolution_clock::now();
    uint64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(args.GetIsolate(), nanos));
}

void Process::kill(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsNumber()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "PID must be an integer")));
        return;
    }

    int32_t pid = args[0]->Int32Value(context).FromMaybe(0);
    int32_t sig = 15; // SIGTERM
    if (args.Length() > 1) {
        if (args[1]->IsInt32()) {
            sig = args[1]->Int32Value(context).FromMaybe(15);
        } else if (args[1]->IsString()) {
            v8::String::Utf8Value sig_str(p_isolate, args[1]);
            std::string s = *sig_str;
            if (s == "SIGHUP") sig = 1;
            else if (s == "SIGINT") sig = 2;
            else if (s == "SIGQUIT") sig = 3;
            else if (s == "SIGILL") sig = 4;
            else if (s == "SIGTRAP") sig = 5;
            else if (s == "SIGABRT") sig = 6;
            else if (s == "SIGFPE") sig = 8;
            else if (s == "SIGKILL") sig = 9;
            else if (s == "SIGBUS") sig = 10;
            else if (s == "SIGSEGV") sig = 11;
            else if (s == "SIGSYS") sig = 12;
            else if (s == "SIGPIPE") sig = 13;
            else if (s == "SIGALRM") sig = 14;
            else if (s == "SIGTERM") sig = 15;
        }
    }

#ifdef _WIN32
    HANDLE p_process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (p_process != nullptr) {
        TerminateProcess(p_process, static_cast<uint32_t>(sig));
        CloseHandle(p_process);
    }
#else
    ::kill(pid, sig);
#endif

    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, true));
}

void Process::stdoutWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() > 0) {
        v8::String::Utf8Value str(p_isolate, args[0]);
        if (*str) {
            fwrite(*str, 1, str.length(), stdout);
        }
    }
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, true));
}

void Process::stderrWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() > 0) {
        v8::String::Utf8Value str(p_isolate, args[0]);
        if (*str) {
            fwrite(*str, 1, str.length(), stderr);
        }
    }
    args.GetReturnValue().Set(v8::Boolean::New(p_isolate, true));
}

void Process::getTitle(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetConsoleTitleW(buf, MAX_PATH) > 0) {
        std::wstring ws(buf);
        info.GetReturnValue().Set(v8::String::NewFromTwoByte(p_isolate, (const uint16_t*)ws.c_str()).ToLocalChecked());
    } else {
        info.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "z8"));
    }
#else
    info.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "z8"));
#endif
}

void Process::setTitle(v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* p_isolate = info.GetIsolate();
    if (value.IsEmpty() || !value->IsString())
        return;
#ifdef _WIN32
    v8::String::Value v(p_isolate, value);
    SetConsoleTitleW((LPCWSTR)*v);
#endif
}

void Process::umask(const v8::FunctionCallbackInfo<v8::Value>& args) {
#ifdef _WIN32
    if (args.Length() > 0 && args[0]->IsInt32()) {
        int32_t mask = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
        args.GetReturnValue().Set(_umask(mask));
    } else {
        int32_t mask = _umask(0);
        _umask(mask);
        args.GetReturnValue().Set(mask);
    }
#else
    if (args.Length() > 0 && args[0]->IsInt32()) {
        int32_t mask = args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
        args.GetReturnValue().Set((uint32_t)::umask(mask));
    } else {
        mode_t mask = ::umask(0);
        ::umask(mask);
        args.GetReturnValue().Set((uint32_t)mask);
    }
#endif
}

void Process::cpuUsage(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    uint64_t user = 0;
    uint64_t system = 0;

#ifdef _WIN32
    FILETIME creation, exit, kernel, user_ft;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user_ft)) {
        ULARGE_INTEGER u, k;
        u.LowPart = user_ft.dwLowDateTime;
        u.HighPart = user_ft.dwHighDateTime;
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        user = u.QuadPart / 10; // 100ns units to microsecs
        system = k.QuadPart / 10;
    }
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        user = (uint64_t)usage.ru_utime.tv_sec * 1000000ULL + usage.ru_utime.tv_usec;
        system = (uint64_t)usage.ru_stime.tv_sec * 1000000ULL + usage.ru_stime.tv_usec;
    }
#endif

    v8::Local<v8::Object> res = v8::Object::New(p_isolate);
    res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "user"), v8::Number::New(p_isolate, (double)user))
        .Check();
    res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "system"), v8::Number::New(p_isolate, (double)system))
        .Check();

    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Object> prev = args[0].As<v8::Object>();
        double p_user = prev->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "user"))
                            .ToLocalChecked()
                            ->NumberValue(context)
                            .FromMaybe(0);
        double p_system = prev->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "system"))
                              .ToLocalChecked()
                              ->NumberValue(context)
                              .FromMaybe(0);

        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "user"), v8::Number::New(p_isolate, (double)user - p_user))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "system"), v8::Number::New(p_isolate, (double)system - p_system))
            .Check();
    }

    args.GetReturnValue().Set(res);
}

void Process::resourceUsage(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> res = v8::Object::New(p_isolate);

#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "rss"), v8::Number::New(p_isolate, (double)pmc.WorkingSetSize))
            .Check();
    }
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "userCPUTime"),
                 v8::Number::New(p_isolate, (double)usage.ru_utime.tv_sec * 1e6 + usage.ru_utime.tv_usec))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "systemCPUTime"),
                 v8::Number::New(p_isolate, (double)usage.ru_stime.tv_sec * 1e6 + usage.ru_stime.tv_usec))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "maxRSS"), v8::Number::New(p_isolate, (double)usage.ru_maxrss))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "sharedMemorySize"),
                 v8::Number::New(p_isolate, (double)usage.ru_ixrss))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "unsharedDataSize"),
                 v8::Number::New(p_isolate, (double)usage.ru_idrss))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "unsharedStackSize"),
                 v8::Number::New(p_isolate, (double)usage.ru_isrss))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "minorPageFault"),
                 v8::Number::New(p_isolate, (double)usage.ru_minflt))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "majorPageFault"),
                 v8::Number::New(p_isolate, (double)usage.ru_majflt))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "swaps"), v8::Number::New(p_isolate, (double)usage.ru_nswap))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "fsRead"), v8::Number::New(p_isolate, (double)usage.ru_inblock))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "fsWrite"), v8::Number::New(p_isolate, (double)usage.ru_oublock))
            .Check();
        res->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "ipcSent"), v8::Number::New(p_isolate, (double)usage.ru_msgsnd))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "ipcReceived"),
                 v8::Number::New(p_isolate, (double)usage.ru_msgrcv))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "signalsCount"),
                 v8::Number::New(p_isolate, (double)usage.ru_nsignals))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "voluntaryContextSwitches"),
                 v8::Number::New(p_isolate, (double)usage.ru_nvcsw))
            .Check();
        res->Set(context,
                 v8::String::NewFromUtf8Literal(p_isolate, "involuntaryContextSwitches"),
                 v8::Number::New(p_isolate, (double)usage.ru_nivcsw))
            .Check();
    }
#endif
    args.GetReturnValue().Set(res);
}

void Process::stdinRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    if (n > 0) {
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, buf, v8::NewStringType::kNormal, static_cast<int32_t>(n)).ToLocalChecked());
    } else {
        args.GetReturnValue().Set(v8::Null(p_isolate));
    }
}

void Process::on(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(args.This());
}

void Process::once(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(args.This());
}

void Process::off(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(args.This());
}

void Process::emit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(v8::Boolean::New(args.GetIsolate(), false));
}

v8::Local<v8::Object> Process::createEnvObject(v8::Isolate* p_isolate, v8::Local<v8::Context> context) {
    v8::Local<v8::Object> env_obj = v8::Object::New(p_isolate);

    // 1. Load system environment variables
#ifdef _WIN32
    auto set_env_from_system = [&](const char* p_name) {
        char* p_buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&p_buf, &sz, p_name) == 0 && p_buf != nullptr) {
            env_obj
                ->Set(context,
                      v8::String::NewFromUtf8(p_isolate, p_name).ToLocalChecked(),
                      v8::String::NewFromUtf8(p_isolate, p_buf).ToLocalChecked())
                .Check();
            free(p_buf);
        }
    };

    // Common Windows env vars
    const char* common_vars[] = {"PATH", "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "TEMP", "TMP", "USERNAME", "COMPUTERNAME"};
    for (const char* var : common_vars) {
        set_env_from_system(var);
    }
#else
    extern char** environ;
    for (char** p_env = environ; *p_env != nullptr; ++p_env) {
        std::string s(*p_env);
        size_t pos = s.find('=');
        if (pos != std::string::npos) {
            std::string key = s.substr(0, pos);
            std::string val = s.substr(pos + 1);
            env_obj
                ->Set(context,
                      v8::String::NewFromUtf8(p_isolate, key.c_str()).ToLocalChecked(),
                      v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked())
                .Check();
        }
    }
#endif

    // 2. Load .env file (dotenv functionality)
    std::map<std::string, std::string> dotenv = loadDotEnv();
    for (auto const& [key, val] : dotenv) {
        env_obj
            ->Set(context,
                  v8::String::NewFromUtf8(p_isolate, key.c_str()).ToLocalChecked(),
                  v8::String::NewFromUtf8(p_isolate, val.c_str()).ToLocalChecked())
            .Check();
    }

    return env_obj;
}

std::map<std::string, std::string> Process::loadDotEnv() {
    std::map<std::string, std::string> res;
    std::ifstream file(".env");
    if (!file.is_open()) {
        return res;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove leading/trailing whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            // Basic cleanup for key and value
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));

            // Remove quotes if present
            if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
                val = val.substr(1, val.size() - 2);
            }

            res[key] = val;
        }
    }

    return res;
}

std::string Process::getExecPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring ws(buf);
    std::string s(ws.length(), ' ');
    for (size_t i = 0; i < ws.length(); ++i) {
        s[i] = static_cast<char>(ws[i]);
    }
    return s;
#elif __APPLE__
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::string(buf);
    }
    return "";
#else
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
#endif
}

} // namespace module
} // namespace z8
