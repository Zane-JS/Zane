#include "fs.hpp"
#include "../stream/stream.hpp"
#include "../buffer/buffer.hpp"
#include "../../adaptive_io.hpp"
#include <chrono>
#include <fcntl.h> // For O_* constants
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <v8-isolate.h>
#ifdef _WIN32
#include <io.h>
#include <BaseTsd.h>
using ssize_t = SSIZE_T;
#include <random>
#include <sys/utime.h>

static std::string generate_random_string(size_t len) {
    static thread_local std::mt19937 generator(std::random_device{}());
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<int> distribution(0, sizeof(charset) - 2);
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; ++i)
        result += charset[distribution(generator)];
    return result;
}
#include <windows.h>

static int64_t fs_pread(int32_t fd, void* p_buf, size_t count, int64_t offset) {
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD) (offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD) (offset >> 32);
    DWORD bytes_read = 0;
    if (ReadFile(h, p_buf, (DWORD) count, &bytes_read, &ov))
        return (int64_t) bytes_read;
    if (GetLastError() == ERROR_HANDLE_EOF)
        return 0;
    return -1;
}

static int64_t fs_pwrite(int32_t fd, const void* p_buf, size_t count, int64_t offset) {
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD) (offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD) (offset >> 32);
    DWORD bytes_written = 0;
    if (WriteFile(h, p_buf, (DWORD) count, &bytes_written, &ov))
        return (int64_t) bytes_written;
    return -1;
}
#else
#include <unistd.h>
#endif
#include "task_queue.hpp"
#include "thread_pool.hpp"
#include <v8-promise.h>

#ifdef _WIN32
#undef CopyFile
#endif

namespace fs = std::filesystem;

namespace zane {
namespace module {

// Maximum size of a single readFileSync / readFile read. Guards against
// unbounded heap allocation (OOM) and keeps the result safely below INT32_MAX
// so the later static_cast<int32_t> used by V8 string APIs cannot overflow.
static constexpr int64_t kMaxReadFileSize = 512LL * 1024 * 1024; // 512 MB

struct DirData {
    fs::path m_path;
    fs::directory_iterator m_it;
    fs::directory_iterator m_end;
    bool m_closed = false;
    std::mutex m_mutex;

    DirData(const fs::path& p) : m_path(p) {
        std::error_code ec;
        m_it = fs::directory_iterator(p, ec);
        if (ec)
            m_closed = true;
    }
};

struct DirReadCtx {
    DirData* p_data;
    bool m_is_error = false;
    std::string m_error_msg;
    fs::directory_entry m_entry;
    bool m_has_entry = false;
};

// Helper to convert file_time_type to V8 Date
// Helper to convert V8 milliseconds to file_time_type
fs::file_time_type V8MillisecondsToFileTime(double ms) {
    auto system_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(static_cast<int64_t>(ms)));
    return fs::file_time_type::clock::now() + (system_time - std::chrono::system_clock::now());
}

#ifdef _WIN32
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int32_t size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int32_t)utf8.length(), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int32_t)utf8.length(), &wide[0], size);
    return wide;
}

// Detects Windows reserved device names (CON, NUL, PRN, AUX, COM1-9, LPT1-9)
// anywhere in the path. The reservation is by basename ignoring any extension
// (e.g. "CON.txt" is still reserved) and is case-insensitive. Windows matches
// on the portion before the FIRST dot, so "CON.foo.bar" is also CON.
static bool isWindowsReservedName(const char* p_path) {
    static const char* const kReserved[] = {
        "CON", "NUL", "PRN", "AUX",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    // Walk each path component; a device name at any segment (e.g. "foo\\CON"
    // or "CON\\bar") triggers reservation.
    std::string path(p_path);
    size_t start = 0;
    size_t sep;
    while ((sep = path.find_first_of("/\\", start)) != std::string::npos || start < path.size()) {
        std::string comp = (sep != std::string::npos)
            ? path.substr(start, sep - start)
            : path.substr(start);
        if (!comp.empty()) {
            // Compare the part before the first dot (Windows device semantics).
            size_t dot = comp.find('.');
            std::string stem = (dot != std::string::npos) ? comp.substr(0, dot) : comp;
            for (const char* r : kReserved) {
                if (_stricmp(stem.c_str(), r) == 0) return true;
            }
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}
#endif

static inline bool isPathSafe(const char* p_path) {
    if (!p_path || p_path[0] == '\0') return false;
    if (p_path[0] == '/' || p_path[0] == '\\') return false;
    if (p_path[0] != '\0' && p_path[1] == ':') return false;

    for (const char* p = p_path; *p; ++p) {
        if (*p == '.' && *(p + 1) == '.') return false;
    }
#ifdef _WIN32
    // Block Windows reserved device names (CON, NUL, PRN, AUX, COM1-9, LPT1-9).
    // These are reserved by basename without extension (e.g. CON.txt is also
    // reserved) and matched case-insensitively. Opening them can hang on console
    // input (DoS) or silently discard data.
    if (isWindowsReservedName(p_path)) return false;
#endif
    return true;
}

v8::Local<v8::Value> FileTimeToV8Date(v8::Isolate* p_isolate, fs::file_time_type ftime) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    double ms =
        static_cast<double>(std::chrono::time_point_cast<std::chrono::milliseconds>(sctp).time_since_epoch().count());
    return v8::Date::New(p_isolate->GetCurrentContext(), ms).ToLocalChecked();
}

v8::Local<v8::ObjectTemplate> FS::createTemplate(v8::Isolate* p_isolate) {
    static bool buffered = []() {
        AdaptiveIO::setupBuffer(stdout);
        AdaptiveIO::setupBuffer(stderr);
        return true;
    }();

    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);

    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "readFileSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::readFileSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "writeFileSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::writeFileSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "existsSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::existsSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "appendFileSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::appendFileSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "statSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::statSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "mkdirSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::mkdirSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "rmSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::rmSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "rmdirSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::rmdirSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "unlinkSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::unlinkSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "lstatSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::lstatSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "readdirSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::readdirSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "renameSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::renameSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "copyFileSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::copyFileSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "realpathSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::realpathSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "accessSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::accessSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "chmodSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::chmodSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "chownSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::chownSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "fchownSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::fchownSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "lchownSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::lchownSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "utimesSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::utimesSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "readlinkSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::readlinkSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "symlinkSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::symlinkSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "linkSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::linkSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "truncateSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::truncateSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "openSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::openSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "readSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::readSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "writeSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::writeSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "closeSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::closeSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "readvSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::readvSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "writevSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::writevSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "fstatSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::fstatSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "cpSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::cpSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "fchmodSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::fchmodSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "fsyncSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::fsyncSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "fdatasyncSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::fdatasyncSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "ftruncateSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::ftruncateSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "futimesSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::futimesSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "mkdtempSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::mkdtempSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "statfsSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::statfsSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "lutimesSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::lutimesSync));
    tmpl->Set(v8::String::NewFromUtf8(p_isolate, "opendirSync").ToLocalChecked(),
              v8::FunctionTemplate::New(p_isolate, FS::opendirSync));

    // Add fs.constants
    v8::Local<v8::ObjectTemplate> constants_tmpl = v8::ObjectTemplate::New(p_isolate);
    constants_tmpl->Set(p_isolate, "F_OK", v8::Integer::New(p_isolate, 0));
    constants_tmpl->Set(p_isolate, "R_OK", v8::Integer::New(p_isolate, 4));
    constants_tmpl->Set(p_isolate, "W_OK", v8::Integer::New(p_isolate, 2));
    constants_tmpl->Set(p_isolate, "X_OK", v8::Integer::New(p_isolate, 1));
    constants_tmpl->Set(p_isolate, "O_RDONLY", v8::Integer::New(p_isolate, 0));
    constants_tmpl->Set(p_isolate, "O_WRONLY", v8::Integer::New(p_isolate, 1));
    constants_tmpl->Set(p_isolate, "O_RDWR", v8::Integer::New(p_isolate, 2));
    constants_tmpl->Set(p_isolate, "O_CREAT", v8::Integer::New(p_isolate, 0x0100));
    constants_tmpl->Set(p_isolate, "O_EXCL", v8::Integer::New(p_isolate, 0x0200));
    constants_tmpl->Set(p_isolate, "O_TRUNC", v8::Integer::New(p_isolate, 0x0400));
    constants_tmpl->Set(p_isolate, "O_APPEND", v8::Integer::New(p_isolate, 0x0008));

    tmpl->Set(p_isolate, "constants", constants_tmpl);

    // Async methods
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readFile"),
              v8::FunctionTemplate::New(p_isolate, FS::readFile));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeFile"),
              v8::FunctionTemplate::New(p_isolate, FS::writeFile));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "stat"), v8::FunctionTemplate::New(p_isolate, FS::stat));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "unlink"), v8::FunctionTemplate::New(p_isolate, FS::unlink));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "mkdir"), v8::FunctionTemplate::New(p_isolate, FS::mkdir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readdir"), v8::FunctionTemplate::New(p_isolate, FS::readdir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rmdir"), v8::FunctionTemplate::New(p_isolate, FS::rmdir));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rename"), v8::FunctionTemplate::New(p_isolate, FS::rename));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "copyFile"),
              v8::FunctionTemplate::New(p_isolate, FS::copyFile));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "access"), v8::FunctionTemplate::New(p_isolate, FS::access));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "appendFile"),
              v8::FunctionTemplate::New(p_isolate, FS::appendFile));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "realpath"),
              v8::FunctionTemplate::New(p_isolate, FS::realpath));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "chmod"), v8::FunctionTemplate::New(p_isolate, FS::chmod));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "chown"), v8::FunctionTemplate::New(p_isolate, FS::chown));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fchown"), v8::FunctionTemplate::New(p_isolate, FS::fchown));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lchown"), v8::FunctionTemplate::New(p_isolate, FS::lchown));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readlink"),
              v8::FunctionTemplate::New(p_isolate, FS::readlink));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "symlink"), v8::FunctionTemplate::New(p_isolate, FS::symlink));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lstat"), v8::FunctionTemplate::New(p_isolate, FS::lstat));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "utimes"), v8::FunctionTemplate::New(p_isolate, FS::utimes));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "link"), v8::FunctionTemplate::New(p_isolate, FS::link));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "truncate"),
              v8::FunctionTemplate::New(p_isolate, FS::truncate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "open"), v8::FunctionTemplate::New(p_isolate, FS::open));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "read"), v8::FunctionTemplate::New(p_isolate, FS::read));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "write"), v8::FunctionTemplate::New(p_isolate, FS::write));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"), v8::FunctionTemplate::New(p_isolate, FS::close));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readv"), v8::FunctionTemplate::New(p_isolate, FS::readv));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "writev"), v8::FunctionTemplate::New(p_isolate, FS::writev));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fstat"), v8::FunctionTemplate::New(p_isolate, FS::fstat));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rm"), v8::FunctionTemplate::New(p_isolate, FS::rm));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "cp"), v8::FunctionTemplate::New(p_isolate, FS::cp));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fchmod"), v8::FunctionTemplate::New(p_isolate, FS::fchmod));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fsync"), v8::FunctionTemplate::New(p_isolate, FS::fsync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fdatasync"),
              v8::FunctionTemplate::New(p_isolate, FS::fdatasync));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "ftruncate"),
              v8::FunctionTemplate::New(p_isolate, FS::ftruncate));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "futimes"), v8::FunctionTemplate::New(p_isolate, FS::futimes));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "mkdtemp"), v8::FunctionTemplate::New(p_isolate, FS::mkdtemp));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "statfs"), v8::FunctionTemplate::New(p_isolate, FS::statfs));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lutimes"), v8::FunctionTemplate::New(p_isolate, FS::lutimes));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "opendir"), v8::FunctionTemplate::New(p_isolate, FS::opendir));
 
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createReadStream"), v8::FunctionTemplate::New(p_isolate, FS::createReadStream));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "createWriteStream"), v8::FunctionTemplate::New(p_isolate, FS::createWriteStream));

    // Expose fs.promises
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "promises"), createPromisesTemplate(p_isolate));

    return tmpl;
}

v8::Local<v8::ObjectTemplate> FS::createPromisesTemplate(v8::Isolate* p_isolate) {
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(p_isolate);
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readFile"),
              v8::FunctionTemplate::New(p_isolate, FS::readFilePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "writeFile"),
              v8::FunctionTemplate::New(p_isolate, FS::writeFilePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "stat"), v8::FunctionTemplate::New(p_isolate, FS::statPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "unlink"),
              v8::FunctionTemplate::New(p_isolate, FS::unlinkPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "mkdir"),
              v8::FunctionTemplate::New(p_isolate, FS::mkdirPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readdir"),
              v8::FunctionTemplate::New(p_isolate, FS::readdirPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rmdir"),
              v8::FunctionTemplate::New(p_isolate, FS::rmdirPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rename"),
              v8::FunctionTemplate::New(p_isolate, FS::renamePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "copyFile"),
              v8::FunctionTemplate::New(p_isolate, FS::copyFilePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "access"),
              v8::FunctionTemplate::New(p_isolate, FS::accessPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "appendFile"),
              v8::FunctionTemplate::New(p_isolate, FS::appendFilePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "realpath"),
              v8::FunctionTemplate::New(p_isolate, FS::realpathPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "chmod"),
              v8::FunctionTemplate::New(p_isolate, FS::chmodPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readlink"),
              v8::FunctionTemplate::New(p_isolate, FS::readlinkPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "symlink"),
              v8::FunctionTemplate::New(p_isolate, FS::symlinkPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lstat"),
              v8::FunctionTemplate::New(p_isolate, FS::lstatPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "utimes"),
              v8::FunctionTemplate::New(p_isolate, FS::utimesPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "chown"),
              v8::FunctionTemplate::New(p_isolate, FS::chownPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fchown"),
              v8::FunctionTemplate::New(p_isolate, FS::fchownPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lchown"),
              v8::FunctionTemplate::New(p_isolate, FS::lchownPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "link"), v8::FunctionTemplate::New(p_isolate, FS::linkPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "truncate"),
              v8::FunctionTemplate::New(p_isolate, FS::truncatePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "open"), v8::FunctionTemplate::New(p_isolate, FS::openPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fstat"),
              v8::FunctionTemplate::New(p_isolate, FS::fstatPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "rm"), v8::FunctionTemplate::New(p_isolate, FS::rmPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "cp"), v8::FunctionTemplate::New(p_isolate, FS::cpPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fchmod"),
              v8::FunctionTemplate::New(p_isolate, FS::fchmodPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fsync"),
              v8::FunctionTemplate::New(p_isolate, FS::fsyncPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "fdatasync"),
              v8::FunctionTemplate::New(p_isolate, FS::fdatasyncPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "ftruncate"),
              v8::FunctionTemplate::New(p_isolate, FS::ftruncatePromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "futimes"),
              v8::FunctionTemplate::New(p_isolate, FS::futimesPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "mkdtemp"),
              v8::FunctionTemplate::New(p_isolate, FS::mkdtempPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "statfs"),
              v8::FunctionTemplate::New(p_isolate, FS::statfsPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "lutimes"),
              v8::FunctionTemplate::New(p_isolate, FS::lutimesPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "opendir"),
              v8::FunctionTemplate::New(p_isolate, FS::opendirPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readv"),
              v8::FunctionTemplate::New(p_isolate, FS::readvPromise));
    tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "writev"),
              v8::FunctionTemplate::New(p_isolate, FS::writevPromise));
    return tmpl;
}

void FS::readFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    // Zero-overhead validation
    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed (traversal detected)")
                .ToLocalChecked());
        return;
    }

    std::string encoding = "";
    if (args.Length() >= 2 && args[1]->IsString()) {
        v8::String::Utf8Value enc_val(p_isolate, args[1]);
        encoding = *enc_val;
    } else if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> p_options = args[1].As<v8::Object>();
        v8::Local<v8::Value> p_enc_opt;
        if (p_options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "encoding")).ToLocal(&p_enc_opt) &&
            p_enc_opt->IsString()) {
            v8::String::Utf8Value enc_val(p_isolate, p_enc_opt);
            encoding = *enc_val;
        }
    }

    // Pass the raw pointer directly to avoid extra std::string allocation/copy
    std::ifstream file(*path_val, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: ENOENT: no such file or directory").ToLocalChecked());
        return;
    }

    int64_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Guard against unbounded allocation (OOM) and int32 overflow in the V8
    // string API below (which takes int32_t length).
    if (size < 0 || size > kMaxReadFileSize) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: File too large to read into memory").ToLocalChecked());
        return;
    }

    if (encoding == "utf8") {
        std::string content(static_cast<size_t>(size), '\0');
        if (file.read(&content[0], size)) {
            args.GetReturnValue().Set(
                v8::String::NewFromUtf8(
                    p_isolate, content.c_str(), v8::NewStringType::kNormal, static_cast<int32_t>(size))
                    .ToLocalChecked());
        }
    } else {
        v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(p_isolate, size);
        std::shared_ptr<v8::BackingStore> sp_backing_store = ab->GetBackingStore();
        if (file.read(static_cast<char*>(sp_backing_store->Data()), size)) {
            v8::Local<v8::Uint8Array> ui8 = v8::Uint8Array::New(ab, 0, size);
            args.GetReturnValue().Set(ui8);
        }
    }
}

void FS::writeFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 2 || !args[0]->IsString() || (!args[1]->IsString() && !args[1]->IsUint8Array())) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path and data must be strings or Uint8Array")
                .ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed (traversal detected)")
                .ToLocalChecked());
        return;
    }

    const void* p_data = nullptr;
    size_t length = 0;
    v8::String::Utf8Value str_data(p_isolate, args[1]);

    if (args[1]->IsString()) {
        p_data = *str_data;
        length = str_data.length();
    } else {
        v8::Local<v8::Uint8Array> uint8 = args[1].As<v8::Uint8Array>();
        p_data = static_cast<const char*>(uint8->Buffer()->GetBackingStore()->Data()) + uint8->ByteOffset();
        length = uint8->ByteLength();
    }

#ifdef _WIN32
    std::wstring wpath = Utf8ToWide(*path_val);
    HANDLE h_file = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_file == INVALID_HANDLE_VALUE) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not open file for writing (Win32)").ToLocalChecked());
        return;
    }

    DWORD bytes_written = 0;
    if (!WriteFile(h_file, p_data, (DWORD)length, &bytes_written, nullptr)) {
        CloseHandle(h_file);
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Write system call failed").ToLocalChecked());
        return;
    }
    CloseHandle(h_file);
#else
    std::ofstream file(*path_val, std::ios::binary);
    if (!file.is_open()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not open file for writing").ToLocalChecked());
        return;
    }
    file.write(static_cast<const char*>(p_data), length);
    file.close();
#endif
}

void FS::appendFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path and data must be strings").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed (traversal detected)")
                .ToLocalChecked());
        return;
    }

    v8::String::Utf8Value data_val(p_isolate, args[1]);

    std::ofstream file(*path_val, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not open file for appending").ToLocalChecked());
        return;
    }

    file.write(*data_val, data_val.length());
    file.close();
}

static void DirReadSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Object> self = args.This().As<v8::Object>();
    if (self->InternalFieldCount() < 1)
        return;
    auto internal = self->GetInternalField(0);
    auto* p_data = static_cast<DirData*>(v8::Local<v8::External>::Cast(internal)->Value());

    std::lock_guard<std::mutex> lock(p_data->m_mutex);
    if (p_data->m_closed || p_data->m_it == p_data->m_end) {
        args.GetReturnValue().Set(v8::Null(p_isolate));
        return;
    }

    std::error_code ec;
    auto entry = *p_data->m_it;
    p_data->m_it.increment(ec);
    if (ec)
        p_data->m_closed = true;

    args.GetReturnValue().Set(FS::createDirent(p_isolate, entry));
}

static void DirCloseSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Local<v8::Object> self = args.This().As<v8::Object>();
    if (self->InternalFieldCount() < 1)
        return;
    auto internal = self->GetInternalField(0);
    auto* p_data = static_cast<DirData*>(v8::Local<v8::External>::Cast(internal)->Value());

    std::lock_guard<std::mutex> lock(p_data->m_mutex);
    p_data->m_closed = true;
    p_data->m_it = fs::directory_iterator();
}

static void DirRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This().As<v8::Object>();
    if (self->InternalFieldCount() < 1)
        return;
    auto internal = self->GetInternalField(0);
    auto* p_data = static_cast<DirData*>(v8::Local<v8::External>::Cast(internal)->Value());

    v8::Local<v8::Function> p_cb;
    v8::Local<v8::Promise::Resolver> p_resolver;
    bool is_promise = true;

    if (args.Length() > 0 && args[0]->IsFunction()) {
        p_cb = args[0].As<v8::Function>();
        is_promise = false;
    } else {
        if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
            return;
        args.GetReturnValue().Set(p_resolver->GetPromise());
    }

    auto p_ctx = new DirReadCtx();
    p_ctx->p_data = p_data;

    zane::Task* p_task = new zane::Task();
    p_task->m_is_promise = is_promise;
    if (is_promise) {
        p_task->m_resolver.Reset(p_isolate, p_resolver);
    } else {
        p_task->m_callback.Reset(p_isolate, p_cb);
    }

    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<DirReadCtx*>(task->p_data);
        if (task->m_is_promise) {
            auto p_resolver = task->m_resolver.Get(isolate);
            if (p_ctx->m_is_error) {
                p_resolver
                    ->Reject(context,
                             v8::Exception::Error(
                                 v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                    .Check();
            } else if (!p_ctx->m_has_entry) {
                p_resolver->Resolve(context, v8::Null(isolate)).Check();
            } else {
                p_resolver->Resolve(context, FS::createDirent(isolate, p_ctx->m_entry)).Check();
            }
        } else {
            v8::Local<v8::Value> argv[2];
            if (p_ctx->m_is_error) {
                argv[0] =
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
                argv[1] = v8::Null(isolate);
            } else if (!p_ctx->m_has_entry) {
                argv[0] = v8::Null(isolate);
                argv[1] = v8::Null(isolate);
            } else {
                argv[0] = v8::Null(isolate);
                argv[1] = FS::createDirent(isolate, p_ctx->m_entry);
            }
            (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::lock_guard<std::mutex> lock(p_ctx->p_data->m_mutex);
        if (p_ctx->p_data->m_closed || p_ctx->p_data->m_it == p_ctx->p_data->m_end) {
            p_ctx->m_has_entry = false;
        } else {
            std::error_code ec;
            p_ctx->m_entry = *p_ctx->p_data->m_it;
            p_ctx->p_data->m_it.increment(ec);
            if (ec) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = ec.message();
            } else {
                p_ctx->m_has_entry = true;
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

static void DirClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This().As<v8::Object>();
    if (self->InternalFieldCount() < 1)
        return;
    auto internal = self->GetInternalField(0);
    auto* p_data = static_cast<DirData*>(v8::Local<v8::External>::Cast(internal)->Value());

    v8::Local<v8::Function> p_cb;
    v8::Local<v8::Promise::Resolver> p_resolver;
    bool is_promise = true;

    if (args.Length() > 0 && args[0]->IsFunction()) {
        p_cb = args[0].As<v8::Function>();
        is_promise = false;
    } else {
        if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
            return;
        args.GetReturnValue().Set(p_resolver->GetPromise());
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_is_promise = is_promise;
    if (is_promise)
        p_task->m_resolver.Reset(p_isolate, p_resolver);
    else
        p_task->m_callback.Reset(p_isolate, p_cb);

    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        if (task->m_is_promise) {
            task->m_resolver.Get(isolate)->Resolve(context, v8::Undefined(isolate)).Check();
        } else {
            v8::Local<v8::Value> argv[1] = {v8::Null(isolate)};
            (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        }
    };

    ThreadPool::getInstance().enqueue([p_task, p_data]() {
        std::lock_guard<std::mutex> lock(p_data->m_mutex);
        p_data->m_closed = true;
        p_data->m_it = fs::directory_iterator();
        TaskQueue::getInstance().enqueue(p_task);
    });
}

static v8::Local<v8::ObjectTemplate> GetDirTemplate(v8::Isolate* p_isolate) {
    static v8::Persistent<v8::ObjectTemplate> dir_tmpl;
    if (dir_tmpl.IsEmpty()) {
        v8::Local<v8::ObjectTemplate> local_tmpl = v8::ObjectTemplate::New(p_isolate);
        local_tmpl->SetInternalFieldCount(1);

        local_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "readSync"),
                        v8::FunctionTemplate::New(p_isolate, DirReadSync));
        local_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "closeSync"),
                        v8::FunctionTemplate::New(p_isolate, DirCloseSync));
        local_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "read"),
                        v8::FunctionTemplate::New(p_isolate, DirRead));
        local_tmpl->Set(v8::String::NewFromUtf8Literal(p_isolate, "close"),
                        v8::FunctionTemplate::New(p_isolate, DirClose));

        dir_tmpl.Reset(p_isolate, local_tmpl);
    }
    return dir_tmpl.Get(p_isolate);
}

v8::Local<v8::Object> FS::createDirent(v8::Isolate* p_isolate, const fs::directory_entry& entry) {
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> dirent = v8::Object::New(p_isolate);

    dirent
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "name"),
              v8::String::NewFromUtf8(p_isolate, entry.path().filename().string().c_str()).ToLocalChecked())
        .Check();

    auto add_method = [&](const char* name, bool val) {
        dirent
            ->Set(p_context,
                  v8::String::NewFromUtf8(p_isolate, name).ToLocalChecked(),
                  v8::FunctionTemplate::New(
                      p_isolate,
                      [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                          args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                      },
                      v8::Boolean::New(p_isolate, val))
                      ->GetFunction(p_context)
                      .ToLocalChecked())
            .Check();
    };

    add_method("isDirectory", entry.is_directory());
    add_method("isFile", entry.is_regular_file());
    add_method("isSymbolicLink", entry.is_symlink());
    add_method("isBlockDevice", entry.is_block_file());
    add_method("isCharacterDevice", entry.is_character_file());
    add_method("isFIFO", entry.is_fifo());
    add_method("isSocket", entry.is_socket());

    return dirent;
}

v8::Local<v8::Object> FS::createDir(v8::Isolate* p_isolate, const fs::path& path) {
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    v8::Local<v8::ObjectTemplate> tmpl = GetDirTemplate(p_isolate);
    v8::Local<v8::Object> dir_obj = tmpl->NewInstance(p_context).ToLocalChecked();

    auto* p_data = new DirData(path);
    dir_obj->SetInternalField(0, v8::External::New(p_isolate, p_data));

    dir_obj
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "path"),
              v8::String::NewFromUtf8(p_isolate, path.string().c_str()).ToLocalChecked())
        .Check();

    return dir_obj;
}

void FS::existsSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        args.GetReturnValue().Set(false);
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr) {
        args.GetReturnValue().Set(false);
        return;
    }

    // Quick security check before disk hit
    if (!isPathSafe(*path_val)) {
        args.GetReturnValue().Set(false);
        return;
    }

    args.GetReturnValue().Set(fs::exists(*path_val));
}

v8::Local<v8::Object>
FS::createStats(v8::Isolate* p_isolate, const fs::path& path, std::error_code& ec, bool follow_symlink) {
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    auto status = follow_symlink ? fs::status(path, ec) : fs::symlink_status(path, ec);

    v8::Local<v8::Object> stats = v8::Object::New(p_isolate);

    // Add properties
    stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "size"),
              v8::Number::New(
                  p_isolate,
                  static_cast<double>(fs::exists(status) && fs::is_regular_file(status) ? fs::file_size(path, ec) : 0)))
        .Check();

    auto mtime = follow_symlink ? fs::last_write_time(path, ec) : fs::last_write_time(path, ec);
    if (!ec) {
        // Correct conversion to UNIX timestamp (ms)
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        double ms = static_cast<double>(
            std::chrono::time_point_cast<std::chrono::milliseconds>(sctp).time_since_epoch().count());

        stats
            ->Set(p_context,
                  v8::String::NewFromUtf8Literal(p_isolate, "mtime"),
                  v8::Date::New(p_context, ms).ToLocalChecked())
            .Check();
        stats->Set(p_context, v8::String::NewFromUtf8Literal(p_isolate, "mtimeMs"), v8::Number::New(p_isolate, ms))
            .Check();
    }

    // Add methods (Simplified)
    stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "isDirectory"),
              v8::FunctionTemplate::New(
                  p_isolate,
                  [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                      args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                  },
                  v8::Boolean::New(p_isolate, fs::is_directory(status)))
                  ->GetFunction(p_context)
                  .ToLocalChecked())
        .Check();

    stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "isFile"),
              v8::FunctionTemplate::New(
                  p_isolate,
                  [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                      args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                  },
                  v8::Boolean::New(p_isolate, fs::is_regular_file(status)))
                  ->GetFunction(p_context)
                  .ToLocalChecked())
        .Check();

    stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "isSymbolicLink"),
              v8::FunctionTemplate::New(
                  p_isolate,
                  [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                      args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                  },
                  v8::Boolean::New(p_isolate, fs::is_symlink(status)))
                  ->GetFunction(p_context)
                  .ToLocalChecked())
        .Check();

    return stats;
}

void FS::statSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    auto stats = createStats(p_isolate, *path_val, ec, true);

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(stats);
}

void FS::mkdirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    // Equivalent to mkdir -p
    fs::create_directories(*path_val, ec);

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error creating directory: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::rmSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    // Equivalent to rm -rf
    fs::remove_all(*path_val, ec);

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error removing path: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::lstatSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    auto stats = createStats(p_isolate, *path_val, ec, false);

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(stats);
}

void FS::readdirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    if (*path_val == nullptr)
        return;

    if (!isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    bool with_file_types = false;
    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> wft;
        if (options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "withFileTypes")).ToLocal(&wft)) {
            with_file_types = wft->BooleanValue(p_isolate);
        }
    }

    std::vector<v8::Local<v8::Value>> entries;
    for (const auto& entry : fs::directory_iterator(*path_val, ec)) {
        if (with_file_types) {
            entries.push_back(FS::createDirent(p_isolate, entry));
        } else {
            entries.push_back(
                v8::String::NewFromUtf8(p_isolate, entry.path().filename().string().c_str()).ToLocalChecked());
        }
    }

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error reading directory: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(v8::Array::New(p_isolate, entries.data(), static_cast<int32_t>(entries.size())));
}

void FS::renameSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Old and new paths must be strings").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value old_path(p_isolate, args[0]);
    v8::String::Utf8Value new_path(p_isolate, args[1]);

    if (*old_path == nullptr || *new_path == nullptr)
        return;

    if (!isPathSafe(*old_path) || !isPathSafe(*new_path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    fs::rename(*old_path, *new_path, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error renaming: " + ec.message()).c_str()).ToLocalChecked());
    }
}

void FS::copyFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Source and destination paths must be strings")
                .ToLocalChecked());
        return;
    }

    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);

    if (*src == nullptr || *dest == nullptr)
        return;

    if (!isPathSafe(*src) || !isPathSafe(*dest)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    fs::copy_file(*src, *dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error copying file: " + ec.message()).c_str()).ToLocalChecked());
    }
}

void FS::realpathSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    fs::path p = fs::canonical(*path, ec); // Canonical is good for realpath
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error resolving realpath: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, p.string().c_str()).ToLocalChecked());
}

void FS::accessSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    if (!fs::exists(*path, ec)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: ENOENT: no such file or directory").ToLocalChecked());
        return;
    }
}

void FS::rmdirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    fs::remove(*path, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error rmdir: " + ec.message()).c_str()).ToLocalChecked());
    }
}

void FS::unlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    fs::remove(*path, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error unlink: " + ec.message()).c_str()).ToLocalChecked());
    }
}

void FS::chmodSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsInt32()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string and mode must be an integer")
                .ToLocalChecked());
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    int32_t mode = args[1]->Int32Value(p_context).FromMaybe(0);
    std::error_code ec;
    fs::permissions(*path, static_cast<fs::perms>(mode), ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error chmod: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::chownSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
#ifdef _WIN32
    // No-op on Windows to match libuv/Node behavior
    return;
#else
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsInt32() || !args[2]->IsInt32()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid arguments")));
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    uid_t uid = static_cast<uid_t>(args[1]->Int32Value(p_context).FromMaybe(-1));
    gid_t gid = static_cast<gid_t>(args[2]->Int32Value(p_context).FromMaybe(-1));
    if (chown(*path, uid, gid) != 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "chown failed")));
    }
#endif
}

void FS::fchownSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
#ifdef _WIN32
    return;
#else
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsInt32() || !args[1]->IsInt32() || !args[2]->IsInt32()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid arguments")));
        return;
    }
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    uid_t uid = static_cast<uid_t>(args[1]->Int32Value(p_context).FromMaybe(-1));
    gid_t gid = static_cast<gid_t>(args[2]->Int32Value(p_context).FromMaybe(-1));
    if (fchown(fd, uid, gid) != 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fchown failed")));
    }
#endif
}

void FS::lchownSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
#ifdef _WIN32
    return;
#else
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsInt32() || !args[2]->IsInt32()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid arguments")));
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    uid_t uid = static_cast<uid_t>(args[1]->Int32Value(p_context).FromMaybe(-1));
    gid_t gid = static_cast<gid_t>(args[2]->Int32Value(p_context).FromMaybe(-1));
    if (lchown(*path, uid, gid) != 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "lchown failed")));
    }
#endif
}

void FS::utimesSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsNumber()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string, atime and mtime must be numbers")
                .ToLocalChecked());
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

    std::error_code ec;
    // Note: std::filesystem only supports last_write_time (mtime), not atime directly.
    // For full utimes functionality, platform-specific APIs would be needed.
    fs::last_write_time(*path, V8MillisecondsToFileTime(mtime), ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error setting times: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::readlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    fs::path target = fs::read_symlink(*path, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error readlink: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
    args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, target.string().c_str()).ToLocalChecked());
}

void FS::symlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Target and path must be strings").ToLocalChecked());
        return;
    }
    v8::String::Utf8Value target_val(p_isolate, args[0]);
    v8::String::Utf8Value path_val(p_isolate, args[1]);

    if (*target_val == nullptr || *path_val == nullptr)
        return;

    if (!isPathSafe(*target_val) || !isPathSafe(*path_val)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;

    // Optional 'type' argument (args[2])
    bool is_dir_symlink = false;
    if (args.Length() >= 3 && args[2]->IsString()) {
        v8::String::Utf8Value type_val(p_isolate, args[2]);
        if (*type_val != nullptr) {
            std::string_view type_str(*type_val);
            if (type_str == "dir" || type_str == "junction") {
                is_dir_symlink = true;
            } else if (type_str == "file") {
                is_dir_symlink = false;
            }
        }
    } else {
        // If type is not specified, try to infer from target
        is_dir_symlink = fs::is_directory(*target_val, ec);
        if (ec) {
            ec.clear();
        }
    }

    if (is_dir_symlink) {
        fs::create_directory_symlink(*target_val, *path_val, ec);
    } else {
        fs::create_symlink(*target_val, *path_val, ec);
    }

    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error symlink: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::linkSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Target and path must be strings").ToLocalChecked());
        return;
    }
    v8::String::Utf8Value target(p_isolate, args[0]);
    v8::String::Utf8Value path(p_isolate, args[1]);

    if (*target == nullptr || *path == nullptr)
        return;

    if (!isPathSafe(*target) || !isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    std::error_code ec;
    fs::create_hard_link(*target, *path, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, ("Error link: " + ec.message()).c_str()).ToLocalChecked());
        return;
    }
}

void FS::truncateSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 1 || !args[0]->IsString())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    uintmax_t length = 0;
    if (args.Length() >= 2 && args[1]->IsNumber())
        length = static_cast<uintmax_t>(args[1]->NumberValue(p_isolate->GetCurrentContext()).FromMaybe(0));
    std::error_code ec;
    fs::resize_file(*path, length, ec);
}

void FS::openSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Path must be a string").ToLocalChecked());
        return;
    }

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    int32_t flags = 0;

    if (args.Length() >= 2) {
        if (args[1]->IsInt32()) {
            flags = args[1]->Int32Value(p_context).FromMaybe(0);
        } else if (args[1]->IsString()) {
            v8::String::Utf8Value flags_str(p_isolate, args[1]);
            if (*flags_str != nullptr) {
                std::string_view f(*flags_str);
                if (f == "r")
                    flags = O_RDONLY;
                else if (f == "r+")
                    flags = O_RDWR;
                else if (f == "w")
                    flags = O_WRONLY | O_CREAT | O_TRUNC;
                else if (f == "w+")
                    flags = O_RDWR | O_CREAT | O_TRUNC;
                else if (f == "a")
                    flags = O_WRONLY | O_CREAT | O_APPEND;
                else if (f == "a+")
                    flags = O_RDWR | O_CREAT | O_APPEND;
                else
                    flags = O_RDONLY;
            }
        }
    } else {
        flags = O_RDONLY;
    }

    int32_t mode = 0666;
    if (args.Length() >= 3 && args[2]->IsInt32()) {
        mode = args[2]->Int32Value(p_context).FromMaybe(0666);
    }

#ifdef _WIN32
    // On Windows, we need _O_BINARY usually for node compatibility
    int32_t fd = _open(*path, flags | _O_BINARY, mode);
#else
    int32_t fd = open(*path, flags, mode);
#endif

    if (fd == -1) {
        p_isolate->ThrowException(v8::String::NewFromUtf8(p_isolate, "Error: Could not open file").ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(fd);
}

void FS::readSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsUint8Array()) {
        p_isolate->ThrowException(v8::String::NewFromUtf8(p_isolate, "TypeError: Invalid arguments").ToLocalChecked());
        return;
    }

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();

    size_t offset = 0;
    if (args.Length() >= 3 && args[2]->IsNumber()) {
        offset = static_cast<size_t>(args[2]->NumberValue(p_context).FromMaybe(0));
    }

    size_t length = buffer->ByteLength() - offset;
    if (args.Length() >= 4 && args[3]->IsNumber()) {
        length = static_cast<size_t>(args[3]->NumberValue(p_context).FromMaybe(static_cast<double>(length)));
    }

    int64_t position = -1;
    if (args.Length() >= 5 && args[4]->IsNumber()) {
        position = static_cast<int64_t>(args[4]->NumberValue(p_context).FromMaybe(-1));
    }

    char* p_data = static_cast<char*>(buffer->Buffer()->GetBackingStore()->Data()) + buffer->ByteOffset() + offset;

#ifdef _WIN32
    if (position != -1) {
        _lseeki64(fd, position, SEEK_SET);
    }
    int32_t bytes_read = _read(fd, p_data, static_cast<uint32_t>(length));
#else
    ssize_t bytes_read;
    if (position != -1) {
        bytes_read = pread(fd, p_data, length, position);
    } else {
        bytes_read = read(fd, p_data, length);
    }
#endif

    if (bytes_read == -1) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not read from file").ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(static_cast<int32_t>(bytes_read));
}

void FS::writeSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 2 || !args[0]->IsInt32()) {
        p_isolate->ThrowException(v8::String::NewFromUtf8(p_isolate, "TypeError: Invalid arguments").ToLocalChecked());
        return;
    }

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);

    const char* p_data = nullptr;
    size_t length = 0;
    std::string str_data;

    if (args[1]->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, args[1]);
        str_data = *utf8;
        p_data = str_data.c_str();
        length = str_data.length();
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
        p_data = static_cast<const char*>(buffer->Buffer()->GetBackingStore()->Data()) + buffer->ByteOffset();
        length = buffer->ByteLength();
    } else {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: Data must be a string or Uint8Array").ToLocalChecked());
        return;
    }

    int64_t position = -1;
    if (args.Length() >= 3 && args[2]->IsNumber()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

#ifdef _WIN32
    int32_t bytes_written;
    if (fd == 1 || fd == 2) {
        FILE* p_stream = (fd == 1) ? stdout : stderr;
        bytes_written = static_cast<int32_t>(fwrite(p_data, 1, length, p_stream));
        if (fd == 1) g_stdout_io.flushIfNeeded(stdout);
        else g_stderr_io.flushIfNeeded(stderr);
    } else {
        if (position != -1) {
            _lseeki64(fd, position, SEEK_SET);
        }
        bytes_written = _write(fd, p_data, static_cast<uint32_t>(length));
    }
#else
    ssize_t bytes_written;
    if (fd == 1 || fd == 2) {
        FILE* p_stream = (fd == 1) ? stdout : stderr;
        bytes_written = fwrite(p_data, 1, length, p_stream);
        if (fd == 1) g_stdout_io.flushIfNeeded(stdout);
        else g_stderr_io.flushIfNeeded(stderr);
    } else if (position != -1) {
        bytes_written = pwrite(fd, p_data, length, position);
    } else {
        bytes_written = write(fd, p_data, length);
    }
#endif

    if (bytes_written == -1) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not write to file").ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(static_cast<int32_t>(bytes_written));
}

void FS::closeSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsInt32()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: FD must be an integer").ToLocalChecked());
        return;
    }

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
#ifdef _WIN32
    int32_t result = _close(fd);
#else
    int32_t result = close(fd);
#endif

    if (result == -1) {
        p_isolate->ThrowException(v8::String::NewFromUtf8(p_isolate, "Error: Could not close file").ToLocalChecked());
        return;
    }
}

void FS::readvSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid arguments")));
        return;
    }

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    size_t total_read = 0;
    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
            char* p_data = static_cast<char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
            size_t len = ui8->ByteLength();

            int32_t read_bytes;
#ifdef _WIN32
            if (position != -1) {
                _lseeki64(fd, position + total_read, SEEK_SET);
            }
            read_bytes = _read(fd, p_data, static_cast<uint32_t>(len));
#else
            if (position != -1) {
                read_bytes = pread(fd, p_data, len, position + total_read);
            } else {
                read_bytes = read(fd, p_data, len);
            }
#endif
            if (read_bytes < 0)
                break;
            total_read += read_bytes;
            if (read_bytes == 0 || static_cast<size_t>(read_bytes) < len)
                break;
        }
    }
    args.GetReturnValue().Set(static_cast<int32_t>(total_read));
}

void FS::writevSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Invalid arguments")));
        return;
    }

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    size_t total_written = 0;
    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
            const char* p_data = static_cast<const char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
            size_t len = ui8->ByteLength();

            int32_t written;
#ifdef _WIN32
            if (position != -1) {
                _lseeki64(fd, position + total_written, SEEK_SET);
            }
            written = _write(fd, p_data, static_cast<uint32_t>(len));
#else
            if (position != -1) {
                written = pwrite(fd, p_data, len, position + total_written);
            } else {
                written = write(fd, p_data, len);
            }
#endif
            if (written == -1)
                break;
            total_written += written;
        }
    }
    args.GetReturnValue().Set(static_cast<int32_t>(total_written));
}

// Async Context for ReadFile
struct ReadFileCtx {
    std::string m_path;
    std::string m_encoding;
    std::string m_content;
    std::vector<char> m_binary_content;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::readFile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ReadFileCtx();
    p_ctx->m_path = *path;

    if (args.Length() >= 2 && args[1]->IsString()) {
        v8::String::Utf8Value enc_val(p_isolate, args[1]);
        p_ctx->m_encoding = *enc_val;
    } else if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> p_options = args[1].As<v8::Object>();
        v8::Local<v8::Value> p_enc_opt;
        if (p_options->Get(p_isolate->GetCurrentContext(), v8::String::NewFromUtf8Literal(p_isolate, "encoding"))
                .ToLocal(&p_enc_opt) &&
            p_enc_opt->IsString()) {
            v8::String::Utf8Value enc_val(p_isolate, p_enc_opt);
            p_ctx->m_encoding = *enc_val;
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadFileCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            if (p_ctx->m_encoding == "utf8") {
                argv[1] = v8::String::NewFromUtf8(isolate,
                                                  p_ctx->m_binary_content.data(),
                                                  v8::NewStringType::kNormal,
                                                  static_cast<int32_t>(p_ctx->m_binary_content.size()))
                              .ToLocalChecked();
            } else {
                v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, p_ctx->m_binary_content.size());
                memcpy(ab->GetBackingStore()->Data(), p_ctx->m_binary_content.data(), p_ctx->m_binary_content.size());
                argv[1] = v8::Uint8Array::New(ab, 0, p_ctx->m_binary_content.size());
            }
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::ifstream file(p_ctx->m_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        } else {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            if (size < 0 || static_cast<int64_t>(size) > kMaxReadFileSize) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "Error: File too large to read into memory";
            } else {
                p_ctx->m_binary_content.resize(static_cast<size_t>(size));
                file.read(p_ctx->m_binary_content.data(), size);
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::readFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_resolver
            ->Reject(p_context,
                     v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked())
            .Check();
        return;
    }

    auto p_ctx = new ReadFileCtx();
    p_ctx->m_path = *path;

    if (args.Length() >= 2 && args[1]->IsString()) {
        v8::String::Utf8Value enc_val(p_isolate, args[1]);
        p_ctx->m_encoding = *enc_val;
    } else if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> p_options = args[1].As<v8::Object>();
        v8::Local<v8::Value> p_enc_opt;
        if (p_options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "encoding")).ToLocal(&p_enc_opt) &&
            p_enc_opt->IsString()) {
            v8::String::Utf8Value enc_val(p_isolate, p_enc_opt);
            p_ctx->m_encoding = *enc_val;
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadFileCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            if (p_ctx->m_encoding == "utf8") {
                p_resolver
                    ->Resolve(context,
                              v8::String::NewFromUtf8(isolate,
                                                      p_ctx->m_binary_content.data(),
                                                      v8::NewStringType::kNormal,
                                                      static_cast<int32_t>(p_ctx->m_binary_content.size()))
                                  .ToLocalChecked())
                    .Check();
            } else {
                v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, p_ctx->m_binary_content.size());
                memcpy(ab->GetBackingStore()->Data(), p_ctx->m_binary_content.data(), p_ctx->m_binary_content.size());
                p_resolver->Resolve(context, v8::Uint8Array::New(ab, 0, p_ctx->m_binary_content.size())).Check();
            }
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::ifstream file(p_ctx->m_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        } else {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            if (size < 0 || static_cast<int64_t>(size) > kMaxReadFileSize) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "Error: File too large to read into memory";
            } else {
                p_ctx->m_binary_content.resize(static_cast<size_t>(size));
                file.read(p_ctx->m_binary_content.data(), size);
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct WriteFileCtx {
    std::string m_path;
    std::string m_content;
    std::vector<char> m_binary_content;
    bool m_is_binary = false;
    bool m_is_error = false;
    std::string m_error_msg;

    // Zero-copy support
    v8::Global<v8::Uint8Array> m_buffer_keep_alive;
    const void* p_zero_copy_data = nullptr;
    size_t m_zero_copy_len = 0;
};

// --- WriteFile ---

void FS::writeFile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new WriteFileCtx();
    p_ctx->m_path = *path;

    if (args[1]->IsString()) {
        v8::String::Utf8Value content(p_isolate, args[1]);
        p_ctx->m_content = *content;
        p_ctx->m_is_binary = false;
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> uint8 = args[1].As<v8::Uint8Array>();
        p_ctx->m_binary_content.resize(uint8->ByteLength());
        uint8->CopyContents(p_ctx->m_binary_content.data(), uint8->ByteLength());
        p_ctx->m_is_binary = true;
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<WriteFileCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::ofstream file(p_ctx->m_path, std::ios::binary);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file for writing";
        } else {
            if (p_ctx->m_is_binary) {
                file.write(p_ctx->m_binary_content.data(), p_ctx->m_binary_content.size());
            } else {
                file.write(p_ctx->m_content.c_str(), p_ctx->m_content.size());
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::writeFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_resolver
            ->Reject(p_context,
                     v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked())
            .Check();
        return;
    }

    auto p_ctx = new WriteFileCtx();
    p_ctx->m_path = *path;

    if (args[1]->IsString()) {
        v8::String::Utf8Value content(p_isolate, args[1]);
        p_ctx->m_content = *content;
        p_ctx->m_is_binary = false;
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> uint8 = args[1].As<v8::Uint8Array>();
        // Zero-copy: Keep JS object alive and use raw pointer directly in worker thread
        p_ctx->m_buffer_keep_alive.Reset(p_isolate, uint8);
        p_ctx->p_zero_copy_data = static_cast<const char*>(uint8->Buffer()->GetBackingStore()->Data()) + uint8->ByteOffset();
        p_ctx->m_zero_copy_len = uint8->ByteLength();
        p_ctx->m_is_binary = true;
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<WriteFileCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        
        // Cleanup global reference
        if (!p_ctx->m_buffer_keep_alive.IsEmpty()) {
            p_ctx->m_buffer_keep_alive.Reset();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        std::wstring wpath = Utf8ToWide(p_ctx->m_path);
        HANDLE h_file = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        
        if (h_file == INVALID_HANDLE_VALUE) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file for writing (Win32 API)";
        } else {
            const void* p_data = p_ctx->m_is_binary ? p_ctx->p_zero_copy_data : p_ctx->m_content.c_str();
            size_t data_len = p_ctx->m_is_binary ? p_ctx->m_zero_copy_len : p_ctx->m_content.size();
            
            DWORD bytes_written = 0;
            if (!WriteFile(h_file, p_data, (DWORD)data_len, &bytes_written, nullptr)) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "Write error (Win32 API)";
            }
            CloseHandle(h_file);
        }
#else
        std::ofstream file(p_ctx->m_path, std::ios::binary);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file for writing";
        } else {
            if (p_ctx->m_is_binary) {
                file.write(static_cast<const char*>(p_ctx->p_zero_copy_data), p_ctx->m_zero_copy_len);
            } else {
                file.write(p_ctx->m_content.c_str(), p_ctx->m_content.size());
            }
        }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct StatCtx {
    std::string m_path;
    bool m_is_error = false;
    std::string m_error_msg;
    bool m_follow_symlink = true;
    std::error_code m_ec;
};

void FS::stat(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;
    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new StatCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = FS::createStats(isolate, p_ctx->m_path, p_ctx->m_ec, p_ctx->m_follow_symlink);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        if (!fs::exists(p_ctx->m_path, p_ctx->m_ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::statPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_resolver
            ->Reject(p_context,
                     v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked())
            .Check();
        return;
    }

    auto p_ctx = new StatCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, FS::createStats(isolate, p_ctx->m_path, p_ctx->m_ec, p_ctx->m_follow_symlink))
                .Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        if (!fs::exists(p_ctx->m_path, p_ctx->m_ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct UnlinkCtx {
    std::string m_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::unlink(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new UnlinkCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<UnlinkCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        #ifdef _WIN32
        std::wstring wpath = Utf8ToWide(p_ctx->m_path);
        if (!DeleteFileW(wpath.c_str())) {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "DeleteFileW failed";
            } else {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "no such file or directory, unlink " + p_ctx->m_path;
            }
        }
#else
        std::error_code ec;
        if (!fs::remove(p_ctx->m_path, ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec ? ec.message() : "Failed to unlink file";
        }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::unlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_resolver
            ->Reject(p_context,
                     v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked())
            .Check();
        return;
    }

    auto p_ctx = new UnlinkCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<UnlinkCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        #ifdef _WIN32
        std::wstring wpath = Utf8ToWide(p_ctx->m_path);
        if (!DeleteFileW(wpath.c_str())) {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "DeleteFileW failed";
            } else {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "no such file or directory, unlink " + p_ctx->m_path;
            }
        }
#else
        std::error_code ec;
        if (!fs::remove(p_ctx->m_path, ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec ? ec.message() : "Failed to unlink file";
        }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Mkdir ---
struct MkdirCtx {
    std::string m_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::mkdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new MkdirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<MkdirCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::create_directories(p_ctx->m_path, ec); // Behaves like mkdir -p
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::mkdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_resolver
            ->Reject(p_context,
                     v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked())
            .Check();
        return;
    }

    auto p_ctx = new MkdirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<MkdirCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        std::wstring wpath = Utf8ToWide(p_ctx->m_path);
        std::error_code ec;
        // In the current test context, we use the simple CreateDirectoryW for non-recursive or existing check
        if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_PATH_NOT_FOUND) {
                // Fallback to recursive if needed
                fs::create_directories(p_ctx->m_path, ec);
                if (ec) {
                    p_ctx->m_is_error = true;
                    p_ctx->m_error_msg = ec.message();
                }
            } else if (err != ERROR_ALREADY_EXISTS) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "CreateDirectoryW failed";
            }
        }
#else
        std::error_code ec;
        fs::create_directories(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Readdir ---
struct ReaddirCtx {
    std::string m_path;
    bool m_with_file_types = false;
    std::vector<fs::directory_entry> m_entries;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::readdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    if (*path == nullptr)
        return;

    if (!isPathSafe(*path)) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "SecurityError: Path validation failed").ToLocalChecked());
        return;
    }

    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ReaddirCtx();
    p_ctx->m_path = *path;

    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> wft;
        if (options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "withFileTypes")).ToLocal(&wft)) {
            p_ctx->m_with_file_types = wft->BooleanValue(p_isolate);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReaddirCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int32_t>(p_ctx->m_entries.size()));
            for (size_t i = 0; i < p_ctx->m_entries.size(); ++i) {
                v8::Local<v8::Value> entry_val;
                if (p_ctx->m_with_file_types) {
                    entry_val = FS::createDirent(isolate, p_ctx->m_entries[i]);
                } else {
                    entry_val = v8::String::NewFromUtf8(isolate, p_ctx->m_entries[i].path().filename().string().c_str())
                                    .ToLocalChecked();
                }
                array->Set(context, static_cast<uint32_t>(i), entry_val).Check();
            }
            argv[1] = array;
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(p_ctx->m_path, ec)) {
            p_ctx->m_entries.push_back(entry);
        }
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::readdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new ReaddirCtx();
    p_ctx->m_path = *path;

    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> wft;
        if (options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "withFileTypes")).ToLocal(&wft)) {
            p_ctx->m_with_file_types = wft->BooleanValue(p_isolate);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReaddirCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int32_t>(p_ctx->m_entries.size()));
            for (size_t i = 0; i < p_ctx->m_entries.size(); ++i) {
                v8::Local<v8::Value> entry_val;
                if (p_ctx->m_with_file_types) {
                    entry_val = FS::createDirent(isolate, p_ctx->m_entries[i]);
                } else {
                    entry_val = v8::String::NewFromUtf8(isolate, p_ctx->m_entries[i].path().filename().string().c_str())
                                    .ToLocalChecked();
                }
                array->Set(context, static_cast<uint32_t>(i), entry_val).Check();
            }
            p_resolver->Resolve(context, array).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(p_ctx->m_path, ec)) {
            p_ctx->m_entries.push_back(entry);
        }
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Rmdir ---
struct RmdirCtx {
    std::string m_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::rmdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new RmdirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RmdirCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::remove(p_ctx->m_path, ec); // Standard rmdir
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::rmdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new RmdirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RmdirCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::remove(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Rename ---
struct RenameCtx {
    std::string m_old_path;
    std::string m_new_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::rename(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value old_path(p_isolate, args[0]);
    v8::String::Utf8Value new_path(p_isolate, args[1]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new RenameCtx();
    p_ctx->m_old_path = *old_path;
    p_ctx->m_new_path = *new_path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RenameCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::rename(p_ctx->m_old_path, p_ctx->m_new_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::renamePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value old_path(p_isolate, args[0]);
    v8::String::Utf8Value new_path(p_isolate, args[1]);
    auto p_ctx = new RenameCtx();
    p_ctx->m_old_path = *old_path;
    p_ctx->m_new_path = *new_path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RenameCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::rename(p_ctx->m_old_path, p_ctx->m_new_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- CopyFile ---
struct CopyFileCtx {
    std::string m_src;
    std::string m_dest;
    int32_t m_flags = 0; // Default to overwrite (0)
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::copyFile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new CopyFileCtx();
    p_ctx->m_src = *src;
    p_ctx->m_dest = *dest;

    if (args.Length() > 3 && args[2]->IsNumber()) {
        p_ctx->m_flags = args[2]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<CopyFileCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        auto options = fs::copy_options::overwrite_existing;
        // Need to check flags. Node.js COPYFILE_EXCL = 1, force = 0?
        // Basic impl: always overwrite for now unless we parse flags better
        // TODO: Map Node flags to std::filesystem copy_options

        fs::copy_file(p_ctx->m_src, p_ctx->m_dest, options, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::copyFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);
    auto p_ctx = new CopyFileCtx();
    p_ctx->m_src = *src;
    p_ctx->m_dest = *dest;

    if (args.Length() > 2 && args[2]->IsNumber()) {
        p_ctx->m_flags = args[2]->Int32Value(p_context).FromMaybe(0);
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<CopyFileCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::copy_file(p_ctx->m_src, p_ctx->m_dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Access ---
struct AccessCtx {
    std::string m_path;
    int32_t m_mode = 0; // F_OK
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::access(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new AccessCtx();
    p_ctx->m_path = *path;
    if (args.Length() > 2 && args[1]->IsNumber()) {
        p_ctx->m_mode = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<AccessCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (!fs::exists(p_ctx->m_path, ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        // TODO: Check specific permissions (R_OK, W_OK) if mode > 0
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::accessPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new AccessCtx();
    p_ctx->m_path = *path;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        p_ctx->m_mode = args[1]->Int32Value(p_context).FromMaybe(0);
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<AccessCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (!fs::exists(p_ctx->m_path, ec)) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- AppendFile ---
struct AppendFileCtx {
    std::string m_path;
    std::string m_content;
    std::vector<char> m_binary_content;
    bool m_is_binary = false;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::appendFile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new AppendFileCtx();
    p_ctx->m_path = *path;

    if (args[1]->IsString()) {
        v8::String::Utf8Value content(p_isolate, args[1]);
        p_ctx->m_content = *content;
        p_ctx->m_is_binary = false;
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> uint8 = args[1].As<v8::Uint8Array>();
        p_ctx->m_binary_content.resize(uint8->ByteLength());
        uint8->CopyContents(p_ctx->m_binary_content.data(), uint8->ByteLength());
        p_ctx->m_is_binary = true;
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<AppendFileCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::ofstream file(p_ctx->m_path, std::ios::binary | std::ios::app);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file for appending";
        } else {
            if (p_ctx->m_is_binary) {
                file.write(p_ctx->m_binary_content.data(), p_ctx->m_binary_content.size());
            } else {
                file.write(p_ctx->m_content.c_str(), p_ctx->m_content.size());
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::appendFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new AppendFileCtx();
    p_ctx->m_path = *path;

    if (args[1]->IsString()) {
        v8::String::Utf8Value content(p_isolate, args[1]);
        p_ctx->m_content = *content;
        p_ctx->m_is_binary = false;
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> uint8 = args[1].As<v8::Uint8Array>();
        p_ctx->m_binary_content.resize(uint8->ByteLength());
        uint8->CopyContents(p_ctx->m_binary_content.data(), uint8->ByteLength());
        p_ctx->m_is_binary = true;
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<AppendFileCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::ofstream file(p_ctx->m_path, std::ios::binary | std::ios::app);
        if (!file.is_open()) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file for appending";
        } else {
            if (p_ctx->m_is_binary) {
                file.write(p_ctx->m_binary_content.data(), p_ctx->m_binary_content.size());
            } else {
                file.write(p_ctx->m_content.c_str(), p_ctx->m_content.size());
            }
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Realpath ---
struct RealpathCtx {
    std::string m_path;
    std::string m_result;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::realpath(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new RealpathCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RealpathCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked();
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::path p = fs::canonical(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        } else {
            p_ctx->m_result = p.string();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::realpathPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new RealpathCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RealpathCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked())
                .Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::path p = fs::canonical(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        } else {
            p_ctx->m_result = p.string();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Chmod ---
struct ChmodCtx {
    std::string m_path;
    int32_t m_mode;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::chmod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsNumber() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t mode = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ChmodCtx();
    p_ctx->m_path = *path;
    p_ctx->m_mode = mode;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChmodCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::permissions(p_ctx->m_path, static_cast<fs::perms>(p_ctx->m_mode), ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::chmodPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsNumber())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t mode = args[1]->Int32Value(p_context).FromMaybe(0);

    auto p_ctx = new ChmodCtx();
    p_ctx->m_path = *path;
    p_ctx->m_mode = mode;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChmodCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::permissions(p_ctx->m_path, static_cast<fs::perms>(p_ctx->m_mode), ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Readlink ---
struct ReadlinkCtx {
    std::string m_path;
    std::string m_result;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::readlink(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ReadlinkCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadlinkCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked();
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::path p = fs::read_symlink(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        } else {
            p_ctx->m_result = p.string();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::readlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new ReadlinkCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadlinkCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked())
                .Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::path p = fs::read_symlink(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        } else {
            p_ctx->m_result = p.string();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Symlink ---
struct SymlinkCtx {
    std::string m_target;
    std::string m_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::symlink(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value target(p_isolate, args[0]);
    v8::String::Utf8Value path(p_isolate, args[1]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new SymlinkCtx();
    p_ctx->m_target = *target;
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<SymlinkCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::create_symlink(p_ctx->m_target, p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::symlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value target(p_isolate, args[0]);
    v8::String::Utf8Value path(p_isolate, args[1]);

    auto p_ctx = new SymlinkCtx();
    p_ctx->m_target = *target;
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<SymlinkCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::create_symlink(p_ctx->m_target, p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Lstat ---
void FS::lstat(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new StatCtx();
    p_ctx->m_path = *path;
    p_ctx->m_follow_symlink = false;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = FS::createStats(isolate, p_ctx->m_path, p_ctx->m_ec, p_ctx->m_follow_symlink);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (!fs::exists(p_ctx->m_path, ec) && !fs::is_symlink(fs::symlink_status(p_ctx->m_path, ec))) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::lstatPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new StatCtx();
    p_ctx->m_path = *path;
    p_ctx->m_follow_symlink = false;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, FS::createStats(isolate, p_ctx->m_path, p_ctx->m_ec, p_ctx->m_follow_symlink))
                .Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (!fs::exists(p_ctx->m_path, ec) && !fs::is_symlink(fs::symlink_status(p_ctx->m_path, ec))) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ENOENT: no such file or directory";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Utimes ---
struct UtimesCtx {
    std::string m_path;
    double m_atime;
    double m_mtime;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::utimes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 4 || !args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsNumber() ||
        !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    double atime = args[1]->NumberValue(p_isolate->GetCurrentContext()).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_isolate->GetCurrentContext()).FromMaybe(0);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new UtimesCtx();
    p_ctx->m_path = *path;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<UtimesCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::last_write_time(p_ctx->m_path, V8MillisecondsToFileTime(p_ctx->m_mtime), ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::utimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsNumber())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

    auto p_ctx = new UtimesCtx();
    p_ctx->m_path = *path;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<UtimesCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::last_write_time(p_ctx->m_path, V8MillisecondsToFileTime(p_ctx->m_mtime), ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Link ---
struct LinkCtx {
    std::string m_existing_path;
    std::string m_new_path;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::link(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value existing_path(p_isolate, args[0]);
    v8::String::Utf8Value new_path(p_isolate, args[1]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new LinkCtx();
    p_ctx->m_existing_path = *existing_path;
    p_ctx->m_new_path = *new_path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<LinkCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::create_hard_link(p_ctx->m_existing_path, p_ctx->m_new_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::linkPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value existing_path(p_isolate, args[0]);
    v8::String::Utf8Value new_path(p_isolate, args[1]);

    auto p_ctx = new LinkCtx();
    p_ctx->m_existing_path = *existing_path;
    p_ctx->m_new_path = *new_path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<LinkCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::create_hard_link(p_ctx->m_existing_path, p_ctx->m_new_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

// --- Truncate ---
struct TruncateCtx {
    std::string m_path;
    uintmax_t m_length;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::truncate(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    uintmax_t length = 0;
    v8::Local<v8::Function> p_cb;

    if (args[1]->IsFunction()) {
        p_cb = args[1].As<v8::Function>();
    } else if (args.Length() > 2 && args[1]->IsNumber() && args[2]->IsFunction()) {
        length = static_cast<uintmax_t>(args[1]->NumberValue(p_isolate->GetCurrentContext()).FromMaybe(0));
        p_cb = args[2].As<v8::Function>();
    } else {
        return;
    }

    auto p_ctx = new TruncateCtx();
    p_ctx->m_path = *path;
    p_ctx->m_length = length;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<TruncateCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::resize_file(p_ctx->m_path, p_ctx->m_length, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::truncatePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    uintmax_t length = 0;
    if (args.Length() > 1 && args[1]->IsNumber()) {
        length = static_cast<uintmax_t>(args[1]->NumberValue(p_context).FromMaybe(0));
    }

    auto p_ctx = new TruncateCtx();
    p_ctx->m_path = *path;
    p_ctx->m_length = length;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<TruncateCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::resize_file(p_ctx->m_path, p_ctx->m_length, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fstatSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    auto p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32()) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "TypeError: FD must be an integer").ToLocalChecked());
        return;
    }
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not stat file descriptor").ToLocalChecked());
        return;
    }
#else
    struct stat st;
    if (fstat(fd, &st) != 0) {
        p_isolate->ThrowException(
            v8::String::NewFromUtf8(p_isolate, "Error: Could not stat file descriptor").ToLocalChecked());
        return;
    }
#endif
    v8::Local<v8::Object> p_stats = v8::Object::New(p_isolate);
    p_stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "size"),
              v8::Number::New(p_isolate, static_cast<double>(st.st_size)))
        .Check();
    p_stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "mtimeMs"),
              v8::Number::New(p_isolate, static_cast<double>(st.st_mtime) * 1000))
        .Check();
    bool is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
    bool is_file = (st.st_mode & S_IFMT) == S_IFREG;
    p_stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "isDirectory"),
              v8::FunctionTemplate::New(
                  p_isolate,
                  [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                      args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                  },
                  v8::Boolean::New(p_isolate, is_dir))
                  ->GetFunction(p_context)
                  .ToLocalChecked())
        .Check();
    p_stats
        ->Set(p_context,
              v8::String::NewFromUtf8Literal(p_isolate, "isFile"),
              v8::FunctionTemplate::New(
                  p_isolate,
                  [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                      args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                  },
                  v8::Boolean::New(p_isolate, is_file))
                  ->GetFunction(p_context)
                  .ToLocalChecked())
        .Check();
    args.GetReturnValue().Set(p_stats);
}

struct OpenCtx {
    std::string m_path;
    int32_t m_flags;
    int32_t m_mode;
    int32_t m_result_fd = -1;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::open(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();
    int32_t flags = 0;
    if (args.Length() >= 2) {
        if (args[1]->IsInt32())
            flags = args[1]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(O_RDONLY);
        else if (args[1]->IsString()) {
            v8::String::Utf8Value f_str(p_isolate, args[1]);
            std::string f(*f_str);
            if (f == "r")
                flags = O_RDONLY;
            else if (f == "r+")
                flags = O_RDWR;
            else if (f == "w")
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            else if (f == "w+")
                flags = O_RDWR | O_CREAT | O_TRUNC;
            else if (f == "a")
                flags = O_WRONLY | O_CREAT | O_APPEND;
            else if (f == "a+")
                flags = O_RDWR | O_CREAT | O_APPEND;
            else
                flags = O_RDONLY;
        }
    }
    int32_t mode = 0666;
    if (args.Length() >= 3 && args[2]->IsInt32())
        mode = args[2]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(0666);
    auto p_ctx = new OpenCtx();
    p_ctx->m_path = *path;
    p_ctx->m_flags = flags;
    p_ctx->m_mode = mode;
    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<OpenCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::Integer::New(isolate, p_ctx->m_result_fd);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        p_ctx->m_result_fd = _open(p_ctx->m_path.c_str(), p_ctx->m_flags | _O_BINARY, p_ctx->m_mode);
#else
        p_ctx->m_result_fd = open(p_ctx->m_path.c_str(), p_ctx->m_flags, p_ctx->m_mode);
#endif
        if (p_ctx->m_result_fd == -1) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::openPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;
    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());
    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t flags = O_RDONLY;
    if (args.Length() >= 2 && args[1]->IsInt32())
        flags = args[1]->Int32Value(p_context).FromMaybe(O_RDONLY);
    auto p_ctx = new OpenCtx();
    p_ctx->m_path = *path;
    p_ctx->m_flags = flags;
    p_ctx->m_mode = 0666;
    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<OpenCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error)
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        else
            p_resolver->Resolve(context, v8::Integer::New(isolate, p_ctx->m_result_fd)).Check();
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        p_ctx->m_result_fd = _open(p_ctx->m_path.c_str(), p_ctx->m_flags | _O_BINARY, p_ctx->m_mode);
#else
        p_ctx->m_result_fd = open(p_ctx->m_path.c_str(), p_ctx->m_flags, p_ctx->m_mode);
#endif
        if (p_ctx->m_result_fd == -1) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Could not open file";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct RWCtx {
    int32_t m_fd;
    void* p_buffer_data;
    size_t m_length;
    int64_t m_position;
    int32_t m_result_count = 0;
    bool m_is_error = false;
    std::string m_error_msg;
    v8::Global<v8::Value> m_buffer_keep_alive;
};

void FS::read(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsUint8Array() || !args[args.Length() - 1]->IsFunction())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Uint8Array> ui8 = args[1].As<v8::Uint8Array>();
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();
    size_t offset = 0;
    if (args.Length() >= 3 && args[2]->IsNumber())
        offset = static_cast<size_t>(args[2]->NumberValue(p_context).FromMaybe(0));
    size_t length = ui8->ByteLength() - offset;
    if (args.Length() >= 4 && args[3]->IsNumber())
        length = static_cast<size_t>(args[3]->NumberValue(p_context).FromMaybe(length));
    int64_t position = -1;
    if (args.Length() >= 5 && args[4]->IsNumber())
        position = static_cast<int64_t>(args[4]->NumberValue(p_context).FromMaybe(-1));
    auto p_ctx = new RWCtx();
    p_ctx->m_fd = fd;
    p_ctx->p_buffer_data = static_cast<char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset() + offset;
    p_ctx->m_length = length;
    p_ctx->m_position = position;
    p_ctx->m_buffer_keep_alive.Reset(p_isolate, ui8);
    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RWCtx*>(task->p_data);
        v8::Local<v8::Value> argv[3];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
            argv[2] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::Integer::New(isolate, p_ctx->m_result_count);
            argv[2] = p_ctx->m_buffer_keep_alive.Get(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 3, argv);
        p_ctx->m_buffer_keep_alive.Reset();
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        if (p_ctx->m_position != -1)
            p_ctx->m_result_count = fs_pread(p_ctx->m_fd, p_ctx->p_buffer_data, p_ctx->m_length, p_ctx->m_position);
        else {
#ifdef _WIN32
            p_ctx->m_result_count = _read(p_ctx->m_fd, p_ctx->p_buffer_data, static_cast<uint32_t>(p_ctx->m_length));
#else
            p_ctx->m_result_count = read(p_ctx->m_fd, p_ctx->p_buffer_data, p_ctx->m_length);
#endif
        }
        if (p_ctx->m_result_count == -1) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Read error";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();
    auto p_ctx = new RWCtx();
    p_ctx->m_fd = fd;
    if (args[1]->IsString()) {
        v8::String::Utf8Value utf8(p_isolate, args[1]);
        std::string str = *utf8;
        p_ctx->m_length = str.length();
        p_ctx->p_buffer_data = malloc(p_ctx->m_length);
        memcpy(p_ctx->p_buffer_data, str.c_str(), p_ctx->m_length);
        p_ctx->m_position = -1;
    } else if (args[1]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> ui8 = args[1].As<v8::Uint8Array>();
        p_ctx->m_length = ui8->ByteLength();
        p_ctx->p_buffer_data = static_cast<char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
        p_ctx->m_buffer_keep_alive.Reset(p_isolate, ui8);
        p_ctx->m_position = -1;
        if (args.Length() >= 3 && args[2]->IsNumber())
            p_ctx->m_position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }
    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RWCtx*>(task->p_data);
        v8::Local<v8::Value> argv[3];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
            argv[2] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::Integer::New(isolate, p_ctx->m_result_count);
            if (!p_ctx->m_buffer_keep_alive.IsEmpty())
                argv[2] = p_ctx->m_buffer_keep_alive.Get(isolate);
            else
                argv[2] = v8::Undefined(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 3, argv);
        if (!p_ctx->m_buffer_keep_alive.IsEmpty())
            p_ctx->m_buffer_keep_alive.Reset();
        else
            free(p_ctx->p_buffer_data);
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        if (p_ctx->m_position != -1)
            p_ctx->m_result_count = fs_pwrite(p_ctx->m_fd, p_ctx->p_buffer_data, p_ctx->m_length, p_ctx->m_position);
        else {
#ifdef _WIN32
            if (p_ctx->m_fd == 1 || p_ctx->m_fd == 2) {
                FILE* p_stream = (p_ctx->m_fd == 1) ? stdout : stderr;
                p_ctx->m_result_count = static_cast<int32_t>(fwrite(p_ctx->p_buffer_data, 1, p_ctx->m_length, p_stream));
                if (p_ctx->m_fd == 1) g_stdout_io.flushIfNeeded(stdout);
                else g_stderr_io.flushIfNeeded(stderr);
            } else {
                p_ctx->m_result_count = _write(p_ctx->m_fd, p_ctx->p_buffer_data, static_cast<uint32_t>(p_ctx->m_length));
            }
#else
            if (p_ctx->m_fd == 1 || p_ctx->m_fd == 2) {
                FILE* p_stream = (p_ctx->m_fd == 1) ? stdout : stderr;
                p_ctx->m_result_count = fwrite(p_ctx->p_buffer_data, 1, p_ctx->m_length, p_stream);
                if (p_ctx->m_fd == 1) g_stdout_io.flushIfNeeded(stdout);
                else g_stderr_io.flushIfNeeded(stderr);
            } else {
                p_ctx->m_result_count = write(p_ctx->m_fd, p_ctx->p_buffer_data, p_ctx->m_length);
            }
#endif
        }
        if (p_ctx->m_result_count == -1) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "Write error";
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::close(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    int32_t fd = args[0]->Int32Value(p_isolate->GetCurrentContext()).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();
    struct CloseCtx {
        int32_t m_fd;
        bool m_is_error = false;
    };
    auto p_ctx = new CloseCtx();
    p_ctx->m_fd = fd;
    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<CloseCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error)
            argv[0] = v8::Exception::Error(v8::String::NewFromUtf8(isolate, "Close error").ToLocalChecked());
        else
            argv[0] = v8::Null(isolate);
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        if (_close(p_ctx->m_fd) != 0)
            p_ctx->m_is_error = true;
#else
        if (close(p_ctx->m_fd) != 0)
            p_ctx->m_is_error = true;
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct FstatCtx {
    int32_t m_fd;
    bool m_is_error = false;
    std::string m_error_msg;
    uint64_t m_size;
    double m_mtime;
    bool m_is_dir;
    bool m_is_file;
};

void FS::fstat(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new FstatCtx();
    p_ctx->m_fd = fd;
    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FstatCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            v8::Local<v8::Object> p_stats = v8::Object::New(isolate);
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "size"),
                      v8::Number::New(isolate, (double) p_ctx->m_size))
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "mtimeMs"),
                      v8::Number::New(isolate, p_ctx->m_mtime))
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "isDirectory"),
                      v8::FunctionTemplate::New(
                          isolate,
                          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                              args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                          },
                          v8::Boolean::New(isolate, p_ctx->m_is_dir))
                          ->GetFunction(context)
                          .ToLocalChecked())
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "isFile"),
                      v8::FunctionTemplate::New(
                          isolate,
                          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                              args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                          },
                          v8::Boolean::New(isolate, p_ctx->m_is_file))
                          ->GetFunction(context)
                          .ToLocalChecked())
                .Check();
            argv[1] = p_stats;
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };
    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        struct _stat64 st;
        if (_fstat64(p_ctx->m_fd, &st) == 0) {
            p_ctx->m_size = st.st_size;
            p_ctx->m_mtime = (double) st.st_mtime * 1000;
            p_ctx->m_is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
            p_ctx->m_is_file = (st.st_mode & S_IFMT) == S_IFREG;
        } else {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fstat error";
        }
#else
        struct stat st;
        if (fstat(p_ctx->m_fd, &st) == 0) {
            p_ctx->m_size = st.st_size;
            p_ctx->m_mtime = (double) st.st_mtime * 1000;
            p_ctx->m_is_dir = S_ISDIR(st.st_mode);
            p_ctx->m_is_file = S_ISREG(st.st_mode);
        } else {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fstat error";
        }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fstatPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    auto p_ctx = new FstatCtx();
    p_ctx->m_fd = fd;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FstatCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            v8::Local<v8::Object> p_stats = v8::Object::New(isolate);
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "size"),
                      v8::Number::New(isolate, (double) p_ctx->m_size))
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "mtimeMs"),
                      v8::Number::New(isolate, p_ctx->m_mtime))
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "isDirectory"),
                      v8::FunctionTemplate::New(
                          isolate,
                          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                              args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                          },
                          v8::Boolean::New(isolate, p_ctx->m_is_dir))
                          ->GetFunction(context)
                          .ToLocalChecked())
                .Check();
            p_stats
                ->Set(context,
                      v8::String::NewFromUtf8Literal(isolate, "isFile"),
                      v8::FunctionTemplate::New(
                          isolate,
                          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
                              args.GetReturnValue().Set(args.Data().As<v8::Boolean>());
                          },
                          v8::Boolean::New(isolate, p_ctx->m_is_file))
                          ->GetFunction(context)
                          .ToLocalChecked())
                .Check();
            p_resolver->Resolve(context, p_stats).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        struct _stat64 st;
        if (_fstat64(p_ctx->m_fd, &st) == 0) {
            p_ctx->m_size = st.st_size;
            p_ctx->m_mtime = (double) st.st_mtime * 1000;
            p_ctx->m_is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
            p_ctx->m_is_file = (st.st_mode & S_IFMT) == S_IFREG;
        } else {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fstat error";
        }
#else
        struct stat st;
        if (fstat(p_ctx->m_fd, &st) == 0) {
            p_ctx->m_size = st.st_size;
            p_ctx->m_mtime = (double) st.st_mtime * 1000;
            p_ctx->m_is_dir = S_ISDIR(st.st_mode);
            p_ctx->m_is_file = S_ISREG(st.st_mode);
        } else {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fstat error";
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct RmCtx {
    std::string m_path;
    bool m_recursive = false;
    bool m_force = false;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::rm(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new RmCtx();
    p_ctx->m_path = *path;

    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> recursive_val;
        if (options->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "recursive")).ToLocal(&recursive_val)) {
            p_ctx->m_recursive = recursive_val->BooleanValue(p_isolate);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RmCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (p_ctx->m_recursive) {
            fs::remove_all(p_ctx->m_path, ec);
        } else {
            fs::remove(p_ctx->m_path, ec);
        }
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::rmPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    auto p_ctx = new RmCtx();
    p_ctx->m_path = *path;

    if (args.Length() >= 2 && args[1]->IsObject()) {
        v8::Local<v8::Object> options = args[1].As<v8::Object>();
        v8::Local<v8::Value> recursive_val;
        if (options->Get(p_context, v8::String::NewFromUtf8Literal(p_isolate, "recursive")).ToLocal(&recursive_val)) {
            p_ctx->m_recursive = recursive_val->BooleanValue(p_isolate);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<RmCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        if (p_ctx->m_recursive) {
            fs::remove_all(p_ctx->m_path, ec);
        } else {
            fs::remove(p_ctx->m_path, ec);
        }
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct CopyCtx {
    std::string m_src;
    std::string m_dest;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::cp(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new CopyCtx();
    p_ctx->m_src = *src;
    p_ctx->m_dest = *dest;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<CopyCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::copy(p_ctx->m_src, p_ctx->m_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::cpPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);
    auto p_ctx = new CopyCtx();
    p_ctx->m_src = *src;
    p_ctx->m_dest = *dest;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<CopyCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        fs::copy(p_ctx->m_src, p_ctx->m_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct FsyncCtx {
    int32_t m_fd;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::fsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new FsyncCtx();
    p_ctx->m_fd = fd;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FsyncCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        HANDLE h = (HANDLE) _get_osfhandle(p_ctx->m_fd);
        if (h == INVALID_HANDLE_VALUE || FlushFileBuffers(h) == 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fsync failed";
        }
#else
            if (fsync(p_ctx->m_fd) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "fsync failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fsyncPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    auto p_ctx = new FsyncCtx();
    p_ctx->m_fd = fd;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FsyncCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        HANDLE h = (HANDLE) _get_osfhandle(p_ctx->m_fd);
        if (h == INVALID_HANDLE_VALUE || FlushFileBuffers(h) == 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "fsync failed";
        }
#else
            if (fsync(p_ctx->m_fd) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "fsync failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fdatasync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    fsync(args);
}

void FS::fdatasyncPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    fsyncPromise(args);
}

void FS::fchmod(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Basic async wrapper if needed, but fchmod is rarely used async in Node.js
}

void FS::fchmodPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
}

void FS::cpSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsString()) {
        p_isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(p_isolate, "Source and destination paths must be strings")));
        return;
    }
    v8::String::Utf8Value src(p_isolate, args[0]);
    v8::String::Utf8Value dest(p_isolate, args[1]);
    std::error_code ec;
    fs::copy(*src, *dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ec.message().c_str()).ToLocalChecked()));
    }
}

void FS::fchmodSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsInt32()) {
        p_isolate->ThrowException(
            v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "fd and mode must be integers")));
        return;
    }
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int32_t mode = args[1]->Int32Value(p_context).FromMaybe(0);
#ifdef _WIN32
    (void) fd;
    (void) mode;
    p_isolate->ThrowException(
        v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fchmod not supported on Windows")));
#else
        if (fchmod(fd, mode) != 0) {
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fchmod failed")));
        }
#endif
}

void FS::fsyncSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
#ifdef _WIN32
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || FlushFileBuffers(h) == 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fsync failed")));
    }
#else
        if (fsync(fd) != 0) {
            p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fsync failed")));
        }
#endif
}

void FS::fdatasyncSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
#ifdef _WIN32
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || FlushFileBuffers(h) == 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fdatasync failed")));
    }
#else
#ifdef __APPLE__
        if (fsync(fd) != 0) {
#else
        if (fdatasync(fd) != 0) {
#endif
            p_isolate->ThrowException(
                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "fdatasync failed")));
        }
#endif
}

void FS::ftruncateSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int64_t len = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        len = static_cast<int64_t>(args[1]->NumberValue(p_context).FromMaybe(0));
    }
#ifdef _WIN32
    if (_chsize_s(fd, len) != 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "ftruncate failed")));
    }
#else
        if (ftruncate(fd, len) != 0) {
            p_isolate->ThrowException(
                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "ftruncate failed")));
        }
#endif
}

void FS::futimesSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsInt32() || !args[1]->IsNumber() || !args[2]->IsNumber())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

#ifdef _WIN32
    struct __utimbuf64 buf;
    buf.actime = static_cast<__time64_t>(atime);
    buf.modtime = static_cast<__time64_t>(mtime);
    if (_futime64(fd, &buf) != 0) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "futimes failed")));
    }
#else
        struct timeval tv[2];
        tv[0].tv_sec = static_cast<time_t>(atime);
        tv[0].tv_usec = static_cast<suseconds_t>((atime - tv[0].tv_sec) * 1000000);
        tv[1].tv_sec = static_cast<time_t>(mtime);
        tv[1].tv_usec = static_cast<suseconds_t>((mtime - tv[1].tv_sec) * 1000000);
        if (futimes(fd, tv) != 0) {
            p_isolate->ThrowException(
                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "futimes failed")));
        }
#endif
}

void FS::mkdtempSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsString())
        return;
    v8::String::Utf8Value prefix(p_isolate, args[0]);
    std::string template_str = std::string(*prefix) + "XXXXXX";

#ifdef _WIN32
    // Simple implementation for Windows
    // Simple implementation for Windows
    for (int32_t i = 0; i < 100; ++i) {
        std::string path = std::string(*prefix) + generate_random_string(6);
        std::error_code ec;
        if (fs::create_directory(path, ec)) {
            args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, path.c_str()).ToLocalChecked());
            return;
        }
    }
    p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "mkdtemp failed")));
#else
        std::vector<char> tmpl(template_str.begin(), template_str.end());
        tmpl.push_back('\0');
        char* p_res = mkdtemp(tmpl.data());
        if (p_res == nullptr) {
            p_isolate->ThrowException(
                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "mkdtemp failed")));
        } else {
            args.GetReturnValue().Set(v8::String::NewFromUtf8(p_isolate, p_res).ToLocalChecked());
        }
#endif
}

struct FtruncateCtx {
    int32_t m_fd;
    int64_t m_len;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::ftruncate(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int64_t len = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        len = static_cast<int64_t>(args[1]->NumberValue(p_context).FromMaybe(0));
    }
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new FtruncateCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_len = len;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FtruncateCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        if (_chsize_s(p_ctx->m_fd, p_ctx->m_len) != 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ftruncate failed";
        }
#else
            if (ftruncate(p_ctx->m_fd, p_ctx->m_len) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "ftruncate failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::ftruncatePromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsInt32())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int64_t len = 0;
    if (args.Length() >= 2 && args[1]->IsNumber()) {
        len = static_cast<int64_t>(args[1]->NumberValue(p_context).FromMaybe(0));
    }

    auto p_ctx = new FtruncateCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_len = len;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FtruncateCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        if (_chsize_s(p_ctx->m_fd, p_ctx->m_len) != 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "ftruncate failed";
        }
#else
            if (ftruncate(p_ctx->m_fd, p_ctx->m_len) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "ftruncate failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct FutimesCtx {
    int32_t m_fd;
    double m_atime;
    double m_mtime;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::futimes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 4 || !args[0]->IsInt32() || !args[1]->IsNumber() || !args[2]->IsNumber() ||
        !args[args.Length() - 1]->IsFunction())
        return;
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new FutimesCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FutimesCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        struct __utimbuf64 buf;
        buf.actime = static_cast<__time64_t>(p_ctx->m_atime);
        buf.modtime = static_cast<__time64_t>(p_ctx->m_mtime);
        if (_futime64(p_ctx->m_fd, &buf) != 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "futimes failed";
        }
#else
            struct timeval tv[2];
            tv[0].tv_sec = static_cast<time_t>(p_ctx->m_atime);
            tv[0].tv_usec = static_cast<suseconds_t>((p_ctx->m_atime - tv[0].tv_sec) * 1000000);
            tv[1].tv_sec = static_cast<time_t>(p_ctx->m_mtime);
            tv[1].tv_usec = static_cast<suseconds_t>((p_ctx->m_mtime - tv[1].tv_sec) * 1000000);
            if (futimes(p_ctx->m_fd, tv) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "futimes failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::futimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsInt32() || !args[1]->IsNumber() || !args[2]->IsNumber())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

    auto p_ctx = new FutimesCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<FutimesCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        struct __utimbuf64 buf;
        buf.actime = static_cast<__time64_t>(p_ctx->m_atime);
        buf.modtime = static_cast<__time64_t>(p_ctx->m_mtime);
        if (_futime64(p_ctx->m_fd, &buf) != 0) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "futimes failed";
        }
#else
            struct timeval tv[2];
            tv[0].tv_sec = static_cast<time_t>(p_ctx->m_atime);
            tv[0].tv_usec = static_cast<suseconds_t>((p_ctx->m_atime - tv[0].tv_sec) * 1000000);
            tv[1].tv_sec = static_cast<time_t>(p_ctx->m_mtime);
            tv[1].tv_usec = static_cast<suseconds_t>((p_ctx->m_mtime - tv[1].tv_sec) * 1000000);
            if (futimes(p_ctx->m_fd, tv) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "futimes failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct MkdtempCtx {
    std::string m_prefix;
    std::string m_result;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::mkdtemp(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value prefix(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new MkdtempCtx();
    p_ctx->m_prefix = *prefix;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<MkdtempCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked();
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::string template_str = p_ctx->m_prefix + "XXXXXX";
#ifdef _WIN32
        bool found = false;
        for (int32_t i = 0; i < 100; ++i) {
            std::string path = p_ctx->m_prefix + generate_random_string(6);
            std::error_code ec;
            if (fs::create_directory(path, ec)) {
                p_ctx->m_result = path;
                found = true;
                break;
            }
        }
        if (!found) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "mkdtemp failed";
        }
#else
            char* p_res = mkdtemp(&template_str[0]);
            if (p_res == nullptr) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "mkdtemp failed";
            } else {
                p_ctx->m_result = p_res;
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::mkdtempPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value prefix(p_isolate, args[0]);

    auto p_ctx = new MkdtempCtx();
    p_ctx->m_prefix = *prefix;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<MkdtempCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::String::NewFromUtf8(isolate, p_ctx->m_result.c_str()).ToLocalChecked())
                .Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::string template_str = p_ctx->m_prefix + "XXXXXX";
#ifdef _WIN32
        bool found = false;
        for (int32_t i = 0; i < 100; ++i) {
            std::string path = p_ctx->m_prefix + generate_random_string(6);
            std::error_code ec;
            if (fs::create_directory(path, ec)) {
                p_ctx->m_result = path;
                found = true;
                break;
            }
        }
        if (!found) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "mkdtemp failed";
        }
#else
            char* p_res = mkdtemp(&template_str[0]);
            if (p_res == nullptr) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "mkdtemp failed";
            } else {
                p_ctx->m_result = p_res;
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

static v8::Local<v8::Object> createStatFsObject(v8::Isolate* p_isolate, const std::filesystem::space_info& info) {
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> stats = v8::Object::New(p_isolate);

    // Constants from Node.js docs or typical values
    // type: 0 (unknown)
    // bsize: 4096 (standard default)
    // blocks: capacity / bsize
    // bfree: free / bsize
    // bavail: available / bsize
    // files: 0 (unknown in std::filesystem)
    // ffree: 0 (unknown in std::filesystem)

    // Use BigInt for potentially large values
    uint64_t bsize = 4096;
    uint64_t blocks = info.capacity / bsize;
    uint64_t bfree = info.free / bsize;
    uint64_t bavail = info.available / bsize;

    stats->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "type"), v8::Integer::New(p_isolate, 0)).Check();
    stats
        ->Set(
            context, v8::String::NewFromUtf8Literal(p_isolate, "bsize"), v8::BigInt::NewFromUnsigned(p_isolate, bsize))
        .Check();
    stats
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "blocks"),
              v8::BigInt::NewFromUnsigned(p_isolate, blocks))
        .Check();
    stats
        ->Set(
            context, v8::String::NewFromUtf8Literal(p_isolate, "bfree"), v8::BigInt::NewFromUnsigned(p_isolate, bfree))
        .Check();
    stats
        ->Set(context,
              v8::String::NewFromUtf8Literal(p_isolate, "bavail"),
              v8::BigInt::NewFromUnsigned(p_isolate, bavail))
        .Check();
    stats->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "files"), v8::Integer::New(p_isolate, 0)).Check();
    stats->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "ffree"), v8::Integer::New(p_isolate, 0)).Check();

    return stats;
}

void FS::statfsSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsString())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    std::error_code ec;
    std::filesystem::space_info info = std::filesystem::space(*path, ec);

    if (ec) {
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8(p_isolate, ec.message().c_str()).ToLocalChecked()));
        return;
    }

    args.GetReturnValue().Set(createStatFsObject(p_isolate, info));
}

struct StatFsCtx {
    std::string m_path;
    std::filesystem::space_info m_info;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::statfs(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new StatFsCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatFsCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Undefined(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = createStatFsObject(isolate, p_ctx->m_info);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        p_ctx->m_info = std::filesystem::space(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::statfsPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);

    auto p_ctx = new StatFsCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<StatFsCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, createStatFsObject(isolate, p_ctx->m_info)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        std::error_code ec;
        p_ctx->m_info = std::filesystem::space(p_ctx->m_path, ec);
        if (ec) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = ec.message();
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::lutimesSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsNumber())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

#ifdef _WIN32
    // Windows implementation for lutimes (symbolic link time)
    // We need to open the file with FILE_FLAG_OPEN_REPARSE_POINT to avoid following the link
    HANDLE p_h_file = CreateFile(*path,
                              FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);

    if (p_h_file == INVALID_HANDLE_VALUE) {
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "lutimes failed: Open failed")));
        return;
    }

    // Convert double (seconds since epoch) to FILETIME
    // 1 second = 10,000,000 intervals of 100ns
    // Epoch difference (1601 to 1970) is 11644473600 seconds
    const int64_t epoch_diff = 11644473600LL;
    int64_t atime_int = static_cast<int64_t>(atime * 10000000) + epoch_diff * 10000000;
    int64_t mtime_int = static_cast<int64_t>(mtime * 10000000) + epoch_diff * 10000000;

    FILETIME ftAtime, ftMtime;
    ftAtime.dwLowDateTime = (DWORD) atime_int;
    ftAtime.dwHighDateTime = (DWORD) (atime_int >> 32);
    ftMtime.dwLowDateTime = (DWORD) mtime_int;
    ftMtime.dwHighDateTime = (DWORD) (mtime_int >> 32);

    if (!SetFileTime(p_h_file, nullptr, &ftAtime, &ftMtime)) {
        CloseHandle(p_h_file);
        p_isolate->ThrowException(
            v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "lutimes failed: SetFileTime failed")));
        return;
    }
    CloseHandle(p_h_file);

#else
        struct timeval tv[2];
        tv[0].tv_sec = static_cast<time_t>(atime);
        tv[0].tv_usec = static_cast<suseconds_t>((atime - tv[0].tv_sec) * 1000000);
        tv[1].tv_sec = static_cast<time_t>(mtime);
        tv[1].tv_usec = static_cast<suseconds_t>((mtime - tv[1].tv_sec) * 1000000);
        if (lutimes(*path, tv) != 0) {
            p_isolate->ThrowException(
                v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "lutimes failed")));
        }
#endif
}

struct LutimesCtx {
    std::string m_path;
    double m_atime;
    double m_mtime;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::lutimes(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 4 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new LutimesCtx();
    p_ctx->m_path = *path;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<LutimesCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        HANDLE p_h_file = CreateFile(p_ctx->m_path.c_str(),
                                  FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
        if (p_h_file == INVALID_HANDLE_VALUE) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "lutimes failed: Open failed";
        } else {
            const int64_t epoch_diff = 11644473600LL;
            int64_t atime_int = static_cast<int64_t>(p_ctx->m_atime * 10000000) + epoch_diff * 10000000;
            int64_t mtime_int = static_cast<int64_t>(p_ctx->m_mtime * 10000000) + epoch_diff * 10000000;
            FILETIME ftAtime, ftMtime;
            ftAtime.dwLowDateTime = (DWORD) atime_int;
            ftAtime.dwHighDateTime = (DWORD) (atime_int >> 32);
            ftMtime.dwLowDateTime = (DWORD) mtime_int;
            ftMtime.dwHighDateTime = (DWORD) (mtime_int >> 32);

            if (!SetFileTime(p_h_file, nullptr, &ftAtime, &ftMtime)) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lutimes failed: SetFileTime failed";
            }
            CloseHandle(p_h_file);
        }

#else
            struct timeval tv[2];
            tv[0].tv_sec = static_cast<time_t>(p_ctx->m_atime);
            tv[0].tv_usec = static_cast<suseconds_t>((p_ctx->m_atime - tv[0].tv_sec) * 1000000);
            tv[1].tv_sec = static_cast<time_t>(p_ctx->m_mtime);
            tv[1].tv_usec = static_cast<suseconds_t>((p_ctx->m_mtime - tv[1].tv_sec) * 1000000);
            if (lutimes(p_ctx->m_path.c_str(), tv) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lutimes failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::lutimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    double atime = args[1]->NumberValue(p_context).FromMaybe(0);
    double mtime = args[2]->NumberValue(p_context).FromMaybe(0);

    auto p_ctx = new LutimesCtx();
    p_ctx->m_path = *path;
    p_ctx->m_atime = atime;
    p_ctx->m_mtime = mtime;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<LutimesCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
        HANDLE p_h_file = CreateFile(p_ctx->m_path.c_str(),
                                  FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
        if (p_h_file == INVALID_HANDLE_VALUE) {
            p_ctx->m_is_error = true;
            p_ctx->m_error_msg = "lutimes failed: Open failed";
        } else {
            const int64_t epoch_diff = 11644473600LL;
            int64_t atime_int = static_cast<int64_t>(p_ctx->m_atime * 10000000) + epoch_diff * 10000000;
            int64_t mtime_int = static_cast<int64_t>(p_ctx->m_mtime * 10000000) + epoch_diff * 10000000;
            FILETIME ftAtime, ftMtime;
            ftAtime.dwLowDateTime = (DWORD) atime_int;
            ftAtime.dwHighDateTime = (DWORD) (atime_int >> 32);
            ftMtime.dwLowDateTime = (DWORD) mtime_int;
            ftMtime.dwHighDateTime = (DWORD) (mtime_int >> 32);

            if (!SetFileTime(p_h_file, nullptr, &ftAtime, &ftMtime)) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lutimes failed: SetFileTime failed";
            }
            CloseHandle(p_h_file);
        }

#else
            struct timeval tv[2];
            tv[0].tv_sec = static_cast<time_t>(p_ctx->m_atime);
            tv[0].tv_usec = static_cast<suseconds_t>((p_ctx->m_atime - tv[0].tv_sec) * 1000000);
            tv[1].tv_sec = static_cast<time_t>(p_ctx->m_mtime);
            tv[1].tv_usec = static_cast<suseconds_t>((p_ctx->m_mtime - tv[1].tv_sec) * 1000000);
            if (lutimes(p_ctx->m_path.c_str(), tv) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lutimes failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct ChownCtx {
    std::string m_path;
    int32_t m_fd = -1;
    int32_t m_uid;
    int32_t m_gid;
    bool m_is_fd = false;
    bool m_is_lchown = false;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::chown(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 4 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ChownCtx();
    p_ctx->m_path = *path;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
    // no-op
#else
            if (chown(p_ctx->m_path.c_str(), p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "chown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fchown(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 4 || !args[0]->IsInt32() || !args[args.Length() - 1]->IsFunction())
        return;
    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ChownCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;
    p_ctx->m_is_fd = true;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
    // no-op
#else
            if (fchown(p_ctx->m_fd, p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "fchown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::lchown(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 4 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;
    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    auto p_ctx = new ChownCtx();
    p_ctx->m_path = *path;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;
    p_ctx->m_is_lchown = true;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        v8::Local<v8::Value> argv[1];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
        } else {
            argv[0] = v8::Null(isolate);
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 1, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
    // no-op
#else
            if (lchown(p_ctx->m_path.c_str(), p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lchown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::chownPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);

    auto p_ctx = new ChownCtx();
    p_ctx->m_path = *path;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
#else
            if (chown(p_ctx->m_path.c_str(), p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "chown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::fchownPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsInt32())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);

    auto p_ctx = new ChownCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;
    p_ctx->m_is_fd = true;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
#else
            if (fchown(p_ctx->m_fd, p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "fchown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::lchownPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 3 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);
    int32_t uid = args[1]->Int32Value(p_context).FromMaybe(-1);
    int32_t gid = args[2]->Int32Value(p_context).FromMaybe(-1);

    auto p_ctx = new ChownCtx();
    p_ctx->m_path = *path;
    p_ctx->m_uid = uid;
    p_ctx->m_gid = gid;
    p_ctx->m_is_lchown = true;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ChownCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            p_resolver->Resolve(context, v8::Undefined(isolate)).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
#ifdef _WIN32
#else
            if (lchown(p_ctx->m_path.c_str(), p_ctx->m_uid, p_ctx->m_gid) != 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "lchown failed";
            }
#endif
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::opendirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::HandleScope handle_scope(p_isolate);
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    args.GetReturnValue().Set(FS::createDir(p_isolate, *path));
}

void FS::opendir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    if (args.Length() < 2 || !args[0]->IsString() || !args[args.Length() - 1]->IsFunction())
        return;

    v8::String::Utf8Value path(p_isolate, args[0]);
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    struct OpendirCtx {
        std::string m_path;
    };
    auto p_ctx = new OpendirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<OpendirCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        argv[0] = v8::Null(isolate);
        argv[1] = FS::createDir(isolate, p_ctx->m_path);
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task]() { TaskQueue::getInstance().enqueue(p_task); });
}

void FS::opendirPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 1 || !args[0]->IsString())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    v8::String::Utf8Value path(p_isolate, args[0]);

    struct OpendirCtx {
        std::string m_path;
    };
    auto p_ctx = new OpendirCtx();
    p_ctx->m_path = *path;

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<OpendirCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        p_resolver->Resolve(context, FS::createDir(isolate, p_ctx->m_path)).Check();
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task]() { TaskQueue::getInstance().enqueue(p_task); });
}

struct ReadVCtx {
    int32_t m_fd;
    int64_t m_position;
    std::vector<size_t> m_buffer_lengths;
    std::vector<std::vector<char>> m_results;
    size_t m_total_read = 0;
    bool m_is_error = false;
    std::string m_error_msg;
    v8::Global<v8::Array> m_js_buffers;
};

void FS::readv(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray() || !args[args.Length() - 1]->IsFunction())
        return;

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull() && args[2]->IsNumber()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    auto p_ctx = new ReadVCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_position = position;
    p_ctx->m_js_buffers.Reset(p_isolate, buffers);

    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            p_ctx->m_buffer_lengths.push_back(buf_val.As<v8::Uint8Array>()->ByteLength());
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadVCtx*>(task->p_data);
        v8::Local<v8::Value> argv[3];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Integer::New(isolate, 0);
            argv[2] = v8::Null(isolate);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::Integer::New(isolate, static_cast<int32_t>(p_ctx->m_total_read));

            // Copy results back to JS buffers
            v8::Local<v8::Array> buffers = p_ctx->m_js_buffers.Get(isolate);
            for (uint32_t i = 0; i < p_ctx->m_results.size(); ++i) {
                v8::Local<v8::Value> buf_val;
                if (buffers->Get(context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
                    v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
                    char* p_dst = static_cast<char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
                    memcpy(p_dst, p_ctx->m_results[i].data(), p_ctx->m_results[i].size());
                }
            }
            argv[2] = buffers;
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 3, argv);
        p_ctx->m_js_buffers.Reset();
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        for (size_t len : p_ctx->m_buffer_lengths) {
            std::vector<char> buf(len);
            int32_t read_bytes;
#ifdef _WIN32
            if (p_ctx->m_position != -1) {
                _lseeki64(p_ctx->m_fd, p_ctx->m_position + p_ctx->m_total_read, SEEK_SET);
            }
            read_bytes = _read(p_ctx->m_fd, buf.data(), static_cast<uint32_t>(len));
#else
                if (p_ctx->m_position != -1) {
                    read_bytes = pread(p_ctx->m_fd, buf.data(), len, p_ctx->m_position + p_ctx->m_total_read);
                } else {
                    read_bytes = read(p_ctx->m_fd, buf.data(), len);
                }
#endif
            if (read_bytes < 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "readv failed during operation";
                break;
            }
            if (read_bytes == 0)
                break;
            buf.resize(read_bytes);
            p_ctx->m_results.push_back(std::move(buf));
            p_ctx->m_total_read += read_bytes;
            if (static_cast<size_t>(read_bytes) < len)
                break;
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

struct WriteVCtx {
    int32_t m_fd;
    int64_t m_position;
    std::vector<std::vector<char>> m_buffers;
    size_t m_total_written = 0;
    bool m_is_error = false;
    std::string m_error_msg;
};

void FS::writev(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray() || !args[args.Length() - 1]->IsFunction())
        return;

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();
    v8::Local<v8::Function> p_cb = args[args.Length() - 1].As<v8::Function>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull() && args[2]->IsNumber()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    auto p_ctx = new WriteVCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_position = position;

    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
            const char* p_data = static_cast<const char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
            size_t len = ui8->ByteLength();
            p_ctx->m_buffers.emplace_back(p_data, p_data + len);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_callback.Reset(p_isolate, p_cb);
    p_task->m_is_promise = false;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<WriteVCtx*>(task->p_data);
        v8::Local<v8::Value> argv[2];
        if (p_ctx->m_is_error) {
            argv[0] =
                v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked());
            argv[1] = v8::Integer::New(isolate, 0);
        } else {
            argv[0] = v8::Null(isolate);
            argv[1] = v8::Integer::New(isolate, static_cast<int32_t>(p_ctx->m_total_written));
        }
        (void) task->m_callback.Get(isolate)->Call(context, context->Global(), 2, argv);
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        for (const auto& buf : p_ctx->m_buffers) {
            int32_t written;
#ifdef _WIN32
            if (p_ctx->m_position != -1) {
                _lseeki64(p_ctx->m_fd, p_ctx->m_position + p_ctx->m_total_written, SEEK_SET);
            }
            written = _write(p_ctx->m_fd, buf.data(), static_cast<uint32_t>(buf.size()));
#else
                if (p_ctx->m_position != -1) {
                    written = pwrite(p_ctx->m_fd, buf.data(), buf.size(), p_ctx->m_position + p_ctx->m_total_written);
                } else {
                    written = write(p_ctx->m_fd, buf.data(), buf.size());
                }
#endif
            if (written == -1) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "writev failed during Operation";
                break;
            }
            p_ctx->m_total_written += written;
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::readvPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull() && args[2]->IsNumber()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    auto p_ctx = new ReadVCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_position = position;
    p_ctx->m_js_buffers.Reset(p_isolate, buffers);

    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            p_ctx->m_buffer_lengths.push_back(buf_val.As<v8::Uint8Array>()->ByteLength());
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<ReadVCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            v8::Local<v8::Array> buffers = p_ctx->m_js_buffers.Get(isolate);
            for (uint32_t i = 0; i < p_ctx->m_results.size(); ++i) {
                v8::Local<v8::Value> buf_val;
                if (buffers->Get(context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
                    v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
                    char* p_dst = static_cast<char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
                    memcpy(p_dst, p_ctx->m_results[i].data(), p_ctx->m_results[i].size());
                }
            }

            v8::Local<v8::Object> res = v8::Object::New(isolate);
            res->Set(context,
                     v8::String::NewFromUtf8Literal(isolate, "bytesRead"),
                     v8::Integer::New(isolate, static_cast<int32_t>(p_ctx->m_total_read)))
                .Check();
            res->Set(context, v8::String::NewFromUtf8Literal(isolate, "buffers"), buffers).Check();
            p_resolver->Resolve(context, res).Check();
        }
        p_ctx->m_js_buffers.Reset();
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        for (size_t len : p_ctx->m_buffer_lengths) {
            std::vector<char> buf(len);
            int32_t read_bytes;
#ifdef _WIN32
            if (p_ctx->m_position != -1) {
                _lseeki64(p_ctx->m_fd, p_ctx->m_position + p_ctx->m_total_read, SEEK_SET);
            }
            read_bytes = _read(p_ctx->m_fd, buf.data(), static_cast<uint32_t>(len));
#else
                if (p_ctx->m_position != -1) {
                    read_bytes = pread(p_ctx->m_fd, buf.data(), len, p_ctx->m_position + p_ctx->m_total_read);
                } else {
                    read_bytes = read(p_ctx->m_fd, buf.data(), len);
                }
#endif
            if (read_bytes < 0) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "readv failed during operation";
                break;
            }
            if (read_bytes == 0)
                break;
            buf.resize(read_bytes);
            p_ctx->m_results.push_back(std::move(buf));
            p_ctx->m_total_read += read_bytes;
            if (static_cast<size_t>(read_bytes) < len)
                break;
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}

void FS::writevPromise(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> p_context = p_isolate->GetCurrentContext();
    if (args.Length() < 2 || !args[0]->IsInt32() || !args[1]->IsArray())
        return;

    v8::Local<v8::Promise::Resolver> p_resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&p_resolver))
        return;
    args.GetReturnValue().Set(p_resolver->GetPromise());

    int32_t fd = args[0]->Int32Value(p_context).FromMaybe(-1);
    v8::Local<v8::Array> buffers = args[1].As<v8::Array>();

    int64_t position = -1;
    if (args.Length() >= 3 && !args[2]->IsUndefined() && !args[2]->IsNull() && args[2]->IsNumber()) {
        position = static_cast<int64_t>(args[2]->NumberValue(p_context).FromMaybe(-1));
    }

    auto p_ctx = new WriteVCtx();
    p_ctx->m_fd = fd;
    p_ctx->m_position = position;

    for (uint32_t i = 0; i < buffers->Length(); ++i) {
        v8::Local<v8::Value> buf_val;
        if (buffers->Get(p_context, i).ToLocal(&buf_val) && buf_val->IsUint8Array()) {
            v8::Local<v8::Uint8Array> ui8 = buf_val.As<v8::Uint8Array>();
            const char* p_data = static_cast<const char*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
            size_t len = ui8->ByteLength();
            p_ctx->m_buffers.emplace_back(p_data, p_data + len);
        }
    }

    zane::Task* p_task = new zane::Task();
    p_task->m_resolver.Reset(p_isolate, p_resolver);
    p_task->m_is_promise = true;
    p_task->p_data = p_ctx;
    p_task->m_runner = [](v8::Isolate* isolate, v8::Local<v8::Context> context, Task* task) {
        auto p_ctx = static_cast<WriteVCtx*>(task->p_data);
        auto p_resolver = task->m_resolver.Get(isolate);
        if (p_ctx->m_is_error) {
            p_resolver
                ->Reject(
                    context,
                    v8::Exception::Error(v8::String::NewFromUtf8(isolate, p_ctx->m_error_msg.c_str()).ToLocalChecked()))
                .Check();
        } else {
            v8::Local<v8::Object> res = v8::Object::New(isolate);
            res->Set(context,
                     v8::String::NewFromUtf8Literal(isolate, "bytesWritten"),
                     v8::Integer::New(isolate, static_cast<int32_t>(p_ctx->m_total_written)))
                .Check();
            p_resolver->Resolve(context, res).Check();
        }
        delete p_ctx;
    };

    ThreadPool::getInstance().enqueue([p_task, p_ctx]() {
        for (const auto& buf : p_ctx->m_buffers) {
            int32_t written;
#ifdef _WIN32
            if (p_ctx->m_position != -1) {
                _lseeki64(p_ctx->m_fd, p_ctx->m_position + p_ctx->m_total_written, SEEK_SET);
            }
            written = _write(p_ctx->m_fd, buf.data(), static_cast<uint32_t>(buf.size()));
#else
                if (p_ctx->m_position != -1) {
                    written = pwrite(p_ctx->m_fd, buf.data(), buf.size(), p_ctx->m_position + p_ctx->m_total_written);
                } else {
                    written = write(p_ctx->m_fd, buf.data(), buf.size());
                }
#endif
            if (written == -1) {
                p_ctx->m_is_error = true;
                p_ctx->m_error_msg = "writev failed during Operation";
                break;
            }
            p_ctx->m_total_written += written;
        }
        TaskQueue::getInstance().enqueue(p_task);
    });
}
  
struct ReadStreamInternal : public zane::module::StreamInternal {
    int32_t m_fd = -1;
    bool m_auto_close = true;

    ~ReadStreamInternal() override {
        if (m_auto_close && m_fd != -1) {
#ifdef _WIN32
            _close(m_fd);
#else
            close(m_fd);
#endif
            m_fd = -1;
        }
    }
};

static void readStream_read(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    ReadStreamInternal* p_ctx = static_cast<ReadStreamInternal*>(internal_data.As<v8::External>()->Value());

    if (p_ctx->m_fd == -1 || p_ctx->m_reading) return;
    p_ctx->m_reading = true;

    size_t size = p_ctx->m_high_water_mark;
    if (args.Length() > 0 && args[0]->IsNumber()) {
        size = static_cast<size_t>(args[0]->Uint32Value(context).FromMaybe(static_cast<uint32_t>(size)));
    }

    std::vector<uint8_t> buffer(size);
    int32_t bytes_read;
#ifdef _WIN32
    bytes_read = _read(p_ctx->m_fd, buffer.data(), (uint32_t)size);
#else
    bytes_read = read(p_ctx->m_fd, buffer.data(), size);
#endif

    p_ctx->m_reading = false;

    v8::Local<v8::Value> push_fn;
    if (!self->Get(context, v8::String::NewFromUtf8Literal(p_isolate, "push")).ToLocal(&push_fn) || !push_fn->IsFunction()) {
        return;
    }

    if (bytes_read <= 0) {
        v8::Local<v8::Value> push_argv[] = { v8::Null(p_isolate) };
        (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
        return;
    }

    v8::Local<v8::Uint8Array> ui8 = zane::module::Buffer::createBuffer(p_isolate, bytes_read);
    memcpy(ui8->Buffer()->GetBackingStore()->Data(), buffer.data(), bytes_read);

    v8::Local<v8::Value> push_argv[] = { ui8 };
    (void)push_fn.As<v8::Function>()->Call(context, self, 1, push_argv);
}

void FS::createReadStream(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Path must be a string")));
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    std::string path = *path_val;

    int32_t fd = -1;
#ifdef _WIN32
    fd = _open(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    fd = open(path.c_str(), O_RDONLY);
#endif

    if (fd == -1) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to open file")));
        return;
    }

    auto p_ctx = new ReadStreamInternal();
    p_ctx->m_fd = fd;
    p_ctx->m_is_readable = true;

    v8::Local<v8::FunctionTemplate> readable_tmpl = zane::module::Stream::getReadableTemplate(p_isolate);
    v8::Local<v8::Object> js_obj;
    if (!readable_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocal(&js_obj)) return;
    
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_ctx));
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_read"), v8::FunctionTemplate::New(p_isolate, readStream_read)->GetFunction(context).ToLocalChecked()).Check();

    v8::Global<v8::Object> global_self(p_isolate, js_obj);
    global_self.SetWeak(p_ctx, [](const v8::WeakCallbackInfo<ReadStreamInternal>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(js_obj);
}

struct WriteStreamInternal : public zane::module::StreamInternal {
    int32_t m_fd = -1;
    bool m_auto_close = true;

    ~WriteStreamInternal() override {
        if (m_auto_close && m_fd != -1) {
#ifdef _WIN32
            _close(m_fd);
#else
            close(m_fd);
#endif
            m_fd = -1;
        }
    }
};

static void writeStream_write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();
    v8::Local<v8::Data> internal_data = self->GetInternalField(0);
    if (internal_data.IsEmpty() || !internal_data.As<v8::Value>()->IsExternal()) return;
    WriteStreamInternal* p_ctx = static_cast<WriteStreamInternal*>(internal_data.As<v8::External>()->Value());

    if (p_ctx->m_fd == -1) return;

    if (args.Length() > 0 && args[0]->IsUint8Array()) {
        v8::Local<v8::Uint8Array> ui8 = args[0].As<v8::Uint8Array>();
        const uint8_t* p_data = static_cast<const uint8_t*>(ui8->Buffer()->GetBackingStore()->Data()) + ui8->ByteOffset();
        size_t len = ui8->ByteLength();
#ifdef _WIN32
        _write(p_ctx->m_fd, p_data, (uint32_t)len);
#else
        write(p_ctx->m_fd, p_data, len);
#endif
    }

    if (args.Length() > 2 && args[2]->IsFunction()) {
        v8::Local<v8::Function> cb = args[2].As<v8::Function>();
        (void)cb->Call(context, v8::Null(p_isolate), 0, nullptr);
    }
}

void FS::createWriteStream(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* p_isolate = args.GetIsolate();
    v8::Local<v8::Context> context = p_isolate->GetCurrentContext();

    if (args.Length() < 1 || !args[0]->IsString()) {
        p_isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(p_isolate, "Path must be a string")));
        return;
    }

    v8::String::Utf8Value path_val(p_isolate, args[0]);
    std::string path = *path_val;

    int32_t fd = -1;
#ifdef _WIN32
    fd = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0666);
#else
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif

    if (fd == -1) {
        p_isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(p_isolate, "Failed to open file")));
        return;
    }

    auto p_ctx = new WriteStreamInternal();
    p_ctx->m_fd = fd;
    p_ctx->m_is_writable = true;

    v8::Local<v8::FunctionTemplate> writable_tmpl = zane::module::Stream::getWritableTemplate(p_isolate);
    v8::Local<v8::Object> js_obj;
    if (!writable_tmpl->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocal(&js_obj)) return;
    
    v8::Local<v8::Data> old_internal = js_obj->GetInternalField(0);
    if (!old_internal.IsEmpty() && old_internal->IsValue() && old_internal.As<v8::Value>()->IsExternal()) {
        delete static_cast<zane::module::StreamInternal*>(old_internal.As<v8::External>()->Value());
    }
    js_obj->SetInternalField(0, v8::External::New(p_isolate, p_ctx));
    js_obj->Set(context, v8::String::NewFromUtf8Literal(p_isolate, "_write"), v8::FunctionTemplate::New(p_isolate, writeStream_write)->GetFunction(context).ToLocalChecked()).Check();

    v8::Global<v8::Object> global_self(p_isolate, js_obj);
    global_self.SetWeak(p_ctx, [](const v8::WeakCallbackInfo<WriteStreamInternal>& data) {
        delete data.GetParameter();
    }, v8::WeakCallbackType::kParameter);

    args.GetReturnValue().Set(js_obj);
}

} // namespace module
} // namespace zane

