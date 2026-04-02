#include "console.hpp"
#include "adaptive_io.hpp"
#include "node/util/util.hpp"
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#endif
#include <chrono>
#include <iostream>
#include <map>
#include <signal.h>
#include <stdlib.h>
#include <string>

namespace z8 {
namespace module {

// Global exit/crash handler to rescue logs
static void FlushAll() {
    fflush(stdout);
    fflush(stderr);
}

static void HandleCrash(int32_t sig) {
    FlushAll();
    signal(sig, SIG_DFL);
    raise(sig);
}

int32_t Console::m_indentation_level = 0;

v8::Local<v8::ObjectTemplate> Console::createTemplate(v8::Isolate* p_isolate) {
    static bool buffered = []() {
        AdaptiveIO::setupBuffer(stdout);
        AdaptiveIO::setupBuffer(stderr);

        std::atexit(FlushAll);
        signal(SIGSEGV, HandleCrash);
        signal(SIGABRT, HandleCrash);
        signal(SIGFPE, HandleCrash);
        signal(SIGILL, HandleCrash);
#ifdef _WIN32
        signal(SIGTERM, HandleCrash);
#endif
        return true;
    }();

    v8::Local<v8::ObjectTemplate> console = v8::ObjectTemplate::New(p_isolate);
    console->Set(v8::String::NewFromUtf8(p_isolate, "log").ToLocalChecked(), v8::FunctionTemplate::New(p_isolate, log));
    console->Set(v8::String::NewFromUtf8(p_isolate, "error").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, error));
    console->Set(v8::String::NewFromUtf8(p_isolate, "warn").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, warn));
    console->Set(v8::String::NewFromUtf8(p_isolate, "info").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, info));
    console->Set(v8::String::NewFromUtf8(p_isolate, "assert").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, assert_));
    console->Set(v8::String::NewFromUtf8(p_isolate, "count").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, count));
    console->Set(v8::String::NewFromUtf8(p_isolate, "countReset").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, countReset));
    console->Set(v8::String::NewFromUtf8(p_isolate, "dir").ToLocalChecked(), v8::FunctionTemplate::New(p_isolate, dir));
    console->Set(v8::String::NewFromUtf8(p_isolate, "dirxml").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, log));
    console->Set(v8::String::NewFromUtf8(p_isolate, "group").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, group));
    console->Set(v8::String::NewFromUtf8(p_isolate, "groupCollapsed").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, groupCollapsed));
    console->Set(v8::String::NewFromUtf8(p_isolate, "groupEnd").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, groupEnd));
    console->Set(v8::String::NewFromUtf8(p_isolate, "time").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, time));
    console->Set(v8::String::NewFromUtf8(p_isolate, "timeLog").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, timeLog));
    console->Set(v8::String::NewFromUtf8(p_isolate, "timeEnd").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, timeEnd));
    console->Set(v8::String::NewFromUtf8(p_isolate, "trace").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, trace));
    console->Set(v8::String::NewFromUtf8(p_isolate, "clear").ToLocalChecked(),
                 v8::FunctionTemplate::New(p_isolate, clear));
    return console;
}

void Console::log(const v8::FunctionCallbackInfo<v8::Value>& args) {
    write(args, nullptr);
}

void Console::error(const v8::FunctionCallbackInfo<v8::Value>& args) {
    write(args, "\x1b[31m", true);
}

void Console::warn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    write(args, "\x1b[33m");
}

void Console::info(const v8::FunctionCallbackInfo<v8::Value>& args) {
    write(args, "\x1b[36m");
}

void Console::assert_(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() > 0 && args[0]->BooleanValue(args.GetIsolate())) {
        return;
    }

    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    FILE* p_out = stderr;
    bool use_color = Util::shouldLogWithColors(p_out);

    if (use_color)
        fputs("\x1b[31m", p_out);
    fputs("Assertion failed:", p_out);

    int32_t len = args.Length();
    if (len > 1) {
        fputc(' ', p_out);
        for (int32_t i = 1; i < len; i++) {
            v8::String::Utf8Value utf8(p_isolate, args[i]);
            if (*utf8) {
                fwrite(*utf8, 1, utf8.length(), p_out);
            } else {
                fputs("undefined", p_out);
            }
            if (i < len - 1)
                fputc(' ', p_out);
        }
    } else {
        fputs(" console.assert", p_out);
    }

    if (use_color)
        fputs("\x1b[0m\n", p_out);
    else
        fputc('\n', p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

static std::map<std::string, int32_t> s_console_counts;

void Console::count(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string label = "default";

    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::String::Utf8Value utf8(p_isolate, args[0]);
        if (*utf8)
            label = *utf8;
    }

    int32_t count = ++s_console_counts[label];
    FILE* p_out = stdout;

    // Apply indentation
    for (int32_t i = 0; i < m_indentation_level; i++)
        fputs("  ", p_out);

    std::string output = label + ": " + std::to_string(count) + "\n";
    fwrite(output.c_str(), 1, output.length(), p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::countReset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string label = "default";

    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::String::Utf8Value utf8(p_isolate, args[0]);
        if (*utf8)
            label = *utf8;
    }

    if (s_console_counts.find(label) != s_console_counts.end()) {
        s_console_counts[label] = 0;
    } else {
        std::string msg = "Count for '" + label + "' does not exist\n";
        fwrite(msg.c_str(), 1, msg.length(), stderr);
        g_stderr_io.flushIfNeeded(stderr);
    }
}

// redundant Inspect removed

void Console::dir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() == 0)
        return;
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    int32_t depth = 2;
    bool colors = Util::shouldLogWithColors(stdout);

    if (args.Length() > 1 && args[1]->IsObject()) {
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> depth_val;
        if (options->Get(p_context, v8::String::NewFromUtf8(p_isolate, "depth").ToLocalChecked()).ToLocal(&depth_val)) {
            if (depth_val->IsNull())
                depth = -1;
            else if (depth_val->IsNumber())
                depth = static_cast<int32_t>(depth_val->NumberValue(p_context).FromMaybe(2.0));
        }
        v8::Local<v8::Value> colors_val;
        if (options->Get(p_context, v8::String::NewFromUtf8(p_isolate, "colors").ToLocalChecked())
                .ToLocal(&colors_val)) {
            if (colors_val->IsBoolean())
                colors = colors_val->BooleanValue(p_isolate);
        }
    }

    std::string result = Util::inspectInternal(p_isolate, args[0], depth, 0, colors) + "\n";
    FILE* p_out = stdout;

    // Apply indentation to each line of the result
    std::string indent = "";
    for (int32_t i = 0; i < m_indentation_level; i++)
        indent += "  ";

    // Prepend indent to the first line
    fwrite(indent.c_str(), 1, indent.length(), p_out);

    // For subsequent lines, we should probably handle it within result replacement or just simple replacement
    size_t pos = 0;
    while ((pos = result.find('\n', pos)) != std::string::npos) {
        if (pos + 1 < result.length()) {
            result.insert(pos + 1, indent);
            pos += indent.length() + 1;
        } else {
            break;
        }
    }

    fwrite(result.c_str(), 1, result.length(), p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::group(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() > 0) {
        log(args);
    }
    m_indentation_level++;
}

void Console::groupCollapsed(const v8::FunctionCallbackInfo<v8::Value>& args) {
    group(args); // CLI doesn't support collapse, same as Group
}

void Console::groupEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (m_indentation_level > 0) {
        m_indentation_level--;
    }
}

static std::map<std::string, std::chrono::steady_clock::time_point> s_console_timers;

void Console::time(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string label = "default";
    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::String::Utf8Value utf8(p_isolate, args[0]);
        if (*utf8)
            label = *utf8;
    }
    s_console_timers[label] = std::chrono::steady_clock::now();
}

void Console::timeLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string label = "default";
    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::String::Utf8Value utf8(p_isolate, args[0]);
        if (*utf8)
            label = *utf8;
    }

    auto it = s_console_timers.find(label);
    if (it == s_console_timers.end()) {
        std::string msg = "Timer '" + label + "' does not exist\n";
        fwrite(msg.c_str(), 1, msg.length(), stderr);
        g_stderr_io.flushIfNeeded(stderr);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double, std::milli>(now - it->second).count();

    FILE* p_out = stdout;
    for (int32_t i = 0; i < m_indentation_level; i++)
        fputs("  ", p_out);

    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %.3fms\n", label.c_str(), elapsed);
    fwrite(buf, 1, strlen(buf), p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::timeEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string label = "default";
    if (args.Length() > 0 && !args[0]->IsUndefined()) {
        v8::String::Utf8Value utf8(p_isolate, args[0]);
        if (*utf8)
            label = *utf8;
    }

    auto it = s_console_timers.find(label);
    if (it == s_console_timers.end()) {
        std::string msg = "Timer '" + label + "' does not exist\n";
        fwrite(msg.c_str(), 1, msg.length(), stderr);
        g_stderr_io.flushIfNeeded(stderr);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double, std::milli>(now - it->second).count();
    s_console_timers.erase(it);

    FILE* p_out = stdout;
    for (int32_t i = 0; i < m_indentation_level; i++)
        fputs("  ", p_out);

    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %.3fms\n", label.c_str(), elapsed);
    fwrite(buf, 1, strlen(buf), p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::trace(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    FILE* p_out = stderr;

    // Apply indentation
    for (int32_t i = 0; i < m_indentation_level; i++)
        fputs("  ", p_out);

    fputs("Trace:", p_out);
    int32_t len = args.Length();
    for (int32_t i = 0; i < len; i++) {
        fputc(' ', p_out);
        v8::String::Utf8Value utf8(p_isolate, args[i]);
        if (*utf8) {
            fwrite(*utf8, 1, utf8.length(), p_out);
        } else {
            fputs("undefined", p_out);
        }
    }
    fputc('\n', p_out);

    v8::Local<v8::StackTrace> stack_trace = v8::StackTrace::CurrentStackTrace(p_isolate, 10);
    for (int32_t i = 0; i < stack_trace->GetFrameCount(); i++) {
        v8::Local<v8::StackFrame> frame = stack_trace->GetFrame(p_isolate, i);
        v8::String::Utf8Value script_name(p_isolate, frame->GetScriptName());
        v8::String::Utf8Value function_name(p_isolate, frame->GetFunctionName());
        int32_t line = frame->GetLineNumber();
        int32_t column = frame->GetColumn();

        for (int32_t j = 0; j < m_indentation_level + 1; j++)
            fputs("  ", p_out);

        const char* p_fn_name = *function_name ? *function_name : "(anonymous)";
        const char* p_sn_name = *script_name ? *script_name : "unknown";

        char buf[512];
        snprintf(buf, sizeof(buf), "at %s (%s:%d:%d)\n", p_fn_name, p_sn_name, line, column);
        fwrite(buf, 1, strlen(buf), p_out);
    }

    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::clear(const v8::FunctionCallbackInfo<v8::Value>& args) {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

void Console::write(const v8::FunctionCallbackInfo<v8::Value>& args, const char* p_color_code, bool is_error) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    FILE* p_out = is_error ? stderr : stdout;
    bool use_color = p_color_code && Util::shouldLogWithColors(p_out);
    bool global_colors = Util::shouldLogWithColors(p_out);

    // Apply indentation
    for (int32_t i = 0; i < m_indentation_level; i++)
        fputs("  ", p_out);

    if (use_color)
        fputs(p_color_code, p_out);
    int32_t len = args.Length();
    for (int32_t i = 0; i < len; i++) {
        if (args[i]->IsObject() && !args[i]->IsString() && !args[i]->IsNumber() && !args[i]->IsBoolean() &&
            !args[i]->IsNull()) {
            // Smart inspection for console.log: default depth 2 (like Node)
            std::string inspected = Util::inspectInternal(p_isolate, args[i], 2, 0, global_colors);
            fwrite(inspected.c_str(), 1, inspected.length(), p_out);
        } else {
            v8::String::Utf8Value utf8(p_isolate, args[i]);
            if (*utf8) {
                fwrite(*utf8, 1, utf8.length(), p_out);
            } else {
                fputs("undefined", p_out);
            }
        }
        if (i < len - 1)
            fputc(' ', p_out);
    }
    if (use_color)
        fputs("\x1b[0m\n", p_out);
    else
        fputc('\n', p_out);
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

void Console::adaptiveFlush(FILE* p_out) {
    // Deprecated: Logic moved to AdaptiveIO class
    if (p_out == stderr) g_stderr_io.flushIfNeeded(p_out);
    else g_stdout_io.flushIfNeeded(p_out);
}

} // namespace module
} // namespace z8
