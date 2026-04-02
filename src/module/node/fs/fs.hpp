#ifndef Z8_MODULE_FS_H
#define Z8_MODULE_FS_H

#include "v8.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace z8 {
namespace module {

class FS {
  public:
    static v8::Local<v8::ObjectTemplate> createTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::ObjectTemplate> createPromisesTemplate(v8::Isolate* p_isolate);
    static v8::Local<v8::Object>
    createStats(v8::Isolate* p_isolate, const std::filesystem::path& path, std::error_code& ec, bool follow_symlink);
    static v8::Local<v8::Object> createDirent(v8::Isolate* p_isolate, const std::filesystem::directory_entry& entry);
    static v8::Local<v8::Object> createDir(v8::Isolate* p_isolate, const std::filesystem::path& path);

    // Sync methods
    static void readFileSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeFileSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void existsSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void appendFileSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lstatSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdirSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rmSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rmdirSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readdirSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void renameSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void copyFileSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void realpathSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void accessSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chmodSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chownSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchownSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lchownSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void utimesSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void symlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void linkSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void truncateSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void openSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void closeSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readvSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writevSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fstatSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void cpSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchmodSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fsyncSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fdatasyncSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void ftruncateSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void futimesSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdtempSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statfsSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lutimesSync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void opendirSync(const v8::FunctionCallbackInfo<v8::Value>& args);

    static void readFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rmdirPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void renamePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void copyFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void accessPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void appendFilePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void realpathPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chmodPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void symlinkPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lstatPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void utimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chownPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchownPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lchownPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void linkPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void truncatePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void openPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fstatPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rmPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void cpPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchmodPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fsyncPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fdatasyncPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void ftruncatePromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void futimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdtempPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statfsPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lutimesPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void opendirPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readvPromise(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writevPromise(const v8::FunctionCallbackInfo<v8::Value>& args);

    // Async methods (Callback-based)
    static void readFile(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writeFile(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void stat(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unlink(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readdir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rmdir(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rename(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void copyFile(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void access(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void appendFile(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void realpath(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chmod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void chown(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchown(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lchown(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readlink(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void symlink(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lstat(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void utimes(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void link(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void truncate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void open(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void read(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void write(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void close(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void readv(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void writev(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fstat(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rm(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void cp(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fchmod(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fsync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fdatasync(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void ftruncate(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void futimes(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void mkdtemp(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void statfs(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void lutimes(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void opendir(const v8::FunctionCallbackInfo<v8::Value>& args);
 
    static void createReadStream(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void createWriteStream(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace module
} // namespace z8

#endif