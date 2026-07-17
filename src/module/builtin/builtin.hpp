#ifndef ZANE_BUILTIN_H
#define ZANE_BUILTIN_H

#include "v8.h"

namespace zane {
namespace builtin {

// Initialize and register all builtin modules into the global object
void registerBuiltins(v8::Isolate* p_isolate, v8::Local<v8::Context> context, v8::Local<v8::Object> global);

// Check if any builtin module has active work (for event loop)
bool hasActiveWork();

} // namespace builtin
} // namespace zane

#endif // ZANE_BUILTIN_H
