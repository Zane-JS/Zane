#include "os.hpp"
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
/* break sort */
#include <iphlpapi.h>
#include <process.h>
#include <windows.h>
#include <winreg.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace zane {
namespace module {

#ifdef _WIN32
static std::string wstringToString(const std::wstring& wstr) {
    if (wstr.empty())
        return "";
    int32_t size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int32_t>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string str_to(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int32_t>(wstr.size()), &str_to[0], size_needed, nullptr, nullptr);
    return str_to;
}

static std::string getEnvVar(const char* p_name) {
    char* p_buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&p_buf, &sz, p_name) == 0 && p_buf != nullptr) {
        std::string res(p_buf);
        free(p_buf);
        return res;
    }
    return "";
}

static std::string getCPUModel() {
    HKEY h_key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &h_key) ==
        ERROR_SUCCESS) {
        char buffer[256];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExA(h_key, "ProcessorNameString", nullptr, nullptr, (LPBYTE) buffer, &size) == ERROR_SUCCESS) {
            RegCloseKey(h_key);
            return std::string(buffer);
        }
        RegCloseKey(h_key);
    }
    return "Unknown CPU";
}

static uint32_t getCPUSpeed() {
    HKEY h_key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &h_key) ==
        ERROR_SUCCESS) {
        DWORD speed = 0;
        DWORD size = sizeof(speed);
        if (RegQueryValueExA(h_key, "~MHz", nullptr, nullptr, (LPBYTE) &speed, &size) == ERROR_SUCCESS) {
            RegCloseKey(h_key);
            return (uint32_t) speed;
        }
        RegCloseKey(h_key);
    }
    return 0;
}
#endif

v8::Local<v8::ObjectTemplate> OS::createTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "arch"), v8::FunctionTemplate::New(p_isolate, arch));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "cpus"), v8::FunctionTemplate::New(p_isolate, cpus));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "freemem"), v8::FunctionTemplate::New(p_isolate, freemem));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "homedir"), v8::FunctionTemplate::New(p_isolate, homedir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "hostname"), v8::FunctionTemplate::New(p_isolate, hostname));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "loadavg"), v8::FunctionTemplate::New(p_isolate, loadavg));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "networkInterfaces"),
              v8::FunctionTemplate::New(p_isolate, networkInterfaces));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "platform"), v8::FunctionTemplate::New(p_isolate, platform));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "release"), v8::FunctionTemplate::New(p_isolate, release));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "tmpdir"), v8::FunctionTemplate::New(p_isolate, tmpdir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "totalmem"), v8::FunctionTemplate::New(p_isolate, totalmem));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "type"), v8::FunctionTemplate::New(p_isolate, type));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "uptime"), v8::FunctionTemplate::New(p_isolate, uptime));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "userInfo"), v8::FunctionTemplate::New(p_isolate, userInfo));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "version"), v8::FunctionTemplate::New(p_isolate, version));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "getPriority"),
              v8::FunctionTemplate::New(p_isolate, getPriority));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "setPriority"),
              v8::FunctionTemplate::New(p_isolate, setPriority));

    // Constants
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EOL"), v8::String::NewFromUtf8Literal(p_isolate, "\r\n"));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "devNull"),
              v8::String::NewFromUtf8Literal(p_isolate, "\\\\.\\nul"));

    v8::Local<v8::ObjectTemplate> constants = v8::ObjectTemplate::New(p_isolate);

    // Priority Constants
    v8::Local<v8::ObjectTemplate> priority = v8::ObjectTemplate::New(p_isolate);
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_LOW"), v8::Number::New(p_isolate, 19));
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_BELOW_NORMAL"), v8::Number::New(p_isolate, 10));
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_NORMAL"), v8::Number::New(p_isolate, 0));
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_ABOVE_NORMAL"), v8::Number::New(p_isolate, -10));
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_HIGH"), v8::Number::New(p_isolate, -15));
    priority->Set(v8::String::NewFromUtf8Literal(p_isolate, "PRIORITY_HIGHEST"), v8::Number::New(p_isolate, -20));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "priority"), priority);

    // Signals (Windows basics)
    v8::Local<v8::ObjectTemplate> signals = v8::ObjectTemplate::New(p_isolate);
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGHUP"), v8::Number::New(p_isolate, 1));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGINT"), v8::Number::New(p_isolate, 2));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGILL"), v8::Number::New(p_isolate, 4));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGABRT"), v8::Number::New(p_isolate, 22));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGFPE"), v8::Number::New(p_isolate, 8));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGKILL"), v8::Number::New(p_isolate, 9));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGSEGV"), v8::Number::New(p_isolate, 11));
    signals->Set(v8::String::NewFromUtf8Literal(p_isolate, "SIGTERM"), v8::Number::New(p_isolate, 15));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "signals"), signals);

    // Errno (Basics)
    v8::Local<v8::ObjectTemplate> errno_tmpl = v8::ObjectTemplate::New(p_isolate);
    errno_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EACCES"), v8::Number::New(p_isolate, 13));
    errno_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EADDRINUSE"), v8::Number::New(p_isolate, 100));
    errno_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "EBADF"), v8::Number::New(p_isolate, 9));
    errno_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "ECONNREFUSED"), v8::Number::New(p_isolate, 111));
    constants->Set(v8::String::NewFromUtf8Literal(p_isolate, "errno"), errno_tmpl);

    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "constants"), constants);

    return tmpl;
}

void OS::arch(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);

    const char* p_arch_str = "unknown";
    switch (sys_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        p_arch_str = "x64";
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        p_arch_str = "arm64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        p_arch_str = "ia32";
        break;
    case PROCESSOR_ARCHITECTURE_ARM:
        p_arch_str = "arm";
        break;
    }

    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, p_arch_str).ToLocalChecked());
}

void OS::cpus(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);

    v8::Local<v8::Array> cpus_arr = v8::Array::New(p_isolate, sys_info.dwNumberOfProcessors);

#ifdef _WIN32
    std::string model = getCPUModel();
    uint32_t speed = getCPUSpeed();
#else
    std::string model = "Unknown CPU";
    uint32_t speed = 0;
#endif

    for (DWORD i = 0; i < sys_info.dwNumberOfProcessors; i++) {
        v8::Local<v8::Object> cpu_obj = v8::Object::New(p_isolate);
        cpu_obj
            ->Set(context,
                  v8::String::NewFromUtf8Literal(p_isolate, "model"),
                  v8::String::NewFromUtf8(p_isolate, model.c_str()).ToLocalChecked())
            .Check();
        cpu_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "speed"), v8::Number::New(p_isolate, speed))
            .Check();

        v8::Local<v8::Object> times_obj = v8::Object::New(p_isolate);
        // Times info is complex on Windows (needs GetSystemTimes), using 0 for now
        times_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "user"), v8::Number::New(p_isolate, 0))
            .Check();
        times_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "nice"), v8::Number::New(p_isolate, 0))
            .Check();
        times_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "sys"), v8::Number::New(p_isolate, 0))
            .Check();
        times_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "idle"), v8::Number::New(p_isolate, 0))
            .Check();
        times_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "irq"), v8::Number::New(p_isolate, 0))
            .Check();

        cpu_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "times"), times_obj).Check();
        cpus_arr->Set(context, i, cpu_obj).Check();
    }

    args.GetReturnValue().Set(cpus_arr);
}

void OS::freemem(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    GlobalMemoryStatusEx(&mem_status);
    args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(p_isolate, mem_status.ullAvailPhys));
}

void OS::homedir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string home = getEnvVar("USERPROFILE");
    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, home.c_str()).ToLocalChecked());
}

void OS::hostname(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    char buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) {
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, buffer).ToLocalChecked());
    } else {
        args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "localhost"));
    }
}

void OS::loadavg(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Windows doesn't really have 1, 5, 15 min load avg in the same way as Unix
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Array> arr = v8::Array::New(p_isolate, 3);
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    arr->Set(context, 0, v8::Number::New(p_isolate, 0)).Check();
    arr->Set(context, 1, v8::Number::New(p_isolate, 0)).Check();
    arr->Set(context, 2, v8::Number::New(p_isolate, 0)).Check();
    args.GetReturnValue().Set(arr);
}

void OS::networkInterfaces(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> result = v8::Object::New(p_isolate);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES p_addresses = (IP_ADAPTER_ADDRESSES*) malloc(outBufLen);

    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, p_addresses, &outBufLen) == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES p_curr_addresses = p_addresses;
        while (p_curr_addresses) {
            std::string name = wstringToString(p_curr_addresses->FriendlyName);

            v8::Local<v8::Array> net_arr = v8::Array::New(p_isolate);
            uint32_t index = 0;

            PIP_ADAPTER_UNICAST_ADDRESS p_unicast = p_curr_addresses->FirstUnicastAddress;
            while (p_unicast) {
                sockaddr* p_sa = p_unicast->Address.lpSockaddr;
                char buf[NI_MAXHOST];
                int32_t res = getnameinfo(p_sa,
                                          (p_sa->sa_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6),
                                          buf,
                                          sizeof(buf),
                                          nullptr,
                                          0,
                                          NI_NUMERICHOST);

                if (res == 0) {

                    v8::Local<v8::Object> addr_obj = v8::Object::New(p_isolate);
                    addr_obj
                        ->Set(context,
                              v8::String::NewFromUtf8Literal(p_isolate, "address"),
                              v8::String::NewFromUtf8(p_isolate, buf).ToLocalChecked())
                        .Check();
                    addr_obj
                        ->Set(context,
                              v8::String::NewFromUtf8Literal(p_isolate, "family"),
                              v8::String::NewFromUtf8Literal(p_isolate, (p_sa->sa_family == AF_INET) ? "IPv4" : "IPv6"))
                        .Check();
                    addr_obj
                        ->Set(context,
                              v8::String::NewFromUtf8Literal(p_isolate, "internal"),
                              v8::Boolean::New(p_isolate, p_curr_addresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK))
                        .Check();

                    net_arr->Set(context, index++, addr_obj).Check();
                }
                p_unicast = p_unicast->Next;
            }

            if (index > 0) {
                result->Set(context, v8::String::NewFromUtf8(p_isolate, name.c_str()).ToLocalChecked(), net_arr)
                    .Check();
            }
            p_curr_addresses = p_curr_addresses->Next;
        }
    }
    free(p_addresses);
    WSACleanup();
#endif

    args.GetReturnValue().Set(result);
}

void OS::platform(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "win32"));
}

void OS::release(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    std::string rel = "Unknown";
#ifdef _WIN32
    HMODULE p_h_mod = GetModuleHandleA("ntdll.dll");
    if (p_h_mod) {
        typedef NTSTATUS(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr p_fx_ptr = (RtlGetVersionPtr) GetProcAddress(p_h_mod, "RtlGetVersion");
        if (p_fx_ptr != nullptr) {
            RTL_OSVERSIONINFOW m_osvi = {0};
            m_osvi.dwOSVersionInfoSize = sizeof(m_osvi);
            if (p_fx_ptr(&m_osvi) == 0) {
                rel = std::to_string(m_osvi.dwMajorVersion) + "." + std::to_string(m_osvi.dwMinorVersion) + "." +
                      std::to_string(m_osvi.dwBuildNumber);
            }
        }
    }
#endif
    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, rel.c_str()).ToLocalChecked());
}

void OS::tmpdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    char path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, path)) {
        // Remove trailing backslash if present
        size_t len = strlen(path);
        if (len > 0 && path[len - 1] == '\\')
            path[len - 1] = '\0';
        args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, path).ToLocalChecked());
    } else {
        args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "C:\\Temp"));
    }
}

void OS::totalmem(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    GlobalMemoryStatusEx(&mem_status);
    args.GetReturnValue().Set(v8::BigInt::NewFromUnsigned(p_isolate, mem_status.ullTotalPhys));
}

void OS::type(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "Windows_NT"));
}

void OS::uptime(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::Number::New(p_isolate, (double) (GetTickCount64() / 1000)));
}

void OS::userInfo(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    char name[256];
    DWORD name_size = sizeof(name);
    GetUserNameA(name, &name_size);

    v8::Local<v8::Object> info = v8::Object::New(p_isolate);
    info->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "username"),
              v8::String::NewFromUtf8(p_isolate, name).ToLocalChecked())
        .Check();
    info->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "uid"), v8::Number::New(p_isolate, -1)).Check();
    info->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "gid"), v8::Number::New(p_isolate, -1)).Check();
    info->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "shell"), v8::Null(p_isolate)).Check();
    info->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "homedir"),
              v8::String::NewFromUtf8(p_isolate, getEnvVar("USERPROFILE").c_str()).ToLocalChecked())
        .Check();

    args.GetReturnValue().Set(info);
}

void OS::version(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(p_isolate, "Windows 10/11"));
}

void OS::getPriority(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    DWORD pid = 0;
    if (args.Length() > 0 && args[0]->IsNumber()) {
        pid = args[0]->Uint32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    } else {
        pid = GetCurrentProcessId();
    }

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) {
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Could not open process")));
        return;
    }

    DWORD priorityClass = GetPriorityClass(hProcess);
    CloseHandle(hProcess);

    int32_t priority = 0;
    switch (priorityClass) {
    case IDLE_PRIORITY_CLASS:
        priority = 19;
        break;
    case BELOW_NORMAL_PRIORITY_CLASS:
        priority = 10;
        break;
    case NORMAL_PRIORITY_CLASS:
        priority = 0;
        break;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        priority = -10;
        break;
    case HIGH_PRIORITY_CLASS:
        priority = -15;
        break;
    case REALTIME_PRIORITY_CLASS:
        priority = -20;
        break;
    }

    args.GetReturnValue().Set(v8::Number::New(p_isolate, priority));
}

void OS::setPriority(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    DWORD pid = 0;
    int32_t priority = 0;

    if (args.Length() == 1) {
        pid = GetCurrentProcessId();
        priority = args[0]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    } else if (args.Length() >= 2) {
        pid = args[0]->Uint32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
        priority = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }

    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (priority >= 19)
        priorityClass = IDLE_PRIORITY_CLASS;
    else if (priority >= 10)
        priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    else if (priority >= 0)
        priorityClass = NORMAL_PRIORITY_CLASS;
    else if (priority >= -10)
        priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (priority >= -15)
        priorityClass = HIGH_PRIORITY_CLASS;
    else
        priorityClass = REALTIME_PRIORITY_CLASS;

    HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) {
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Could not open process")));
        return;
    }

    if (!SetPriorityClass(hProcess, priorityClass)) {
        CloseHandle(hProcess);
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Could not set priority class")));
        return;
    }

    CloseHandle(hProcess);
}

} // namespace module
} // namespace zane
