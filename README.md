# Zane

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="asset/icon.png">
  <img alt="Zane" src="asset/icon.png" width="128">
</picture>

**Zane** (/zeɪn/, pronounced zayn) — a V8-first JavaScript runtime for Windows and Linux. Not a Node.js fork. Behavioral compatibility with `node:*` APIs.

## Philosophy: V8-first

Zane embraces V8 as its foundation, not as an implementation detail. Rather than wrapping a Node.js compatibility layer around V8, we build directly on V8's C++ API. This means:

- **No hidden event loop abstraction** — V8's microtask queue is explicit
- **No legacy API baggage** — every API is designed for modern JavaScript
- **Full control over memory and handles** — `v8::Global`, `v8::Persistent`, explicit lifetime management
- **Zero unnecessary abstraction** — what V8 gives, Zane uses directly

## Features

- **Pure C++23** — zero overhead, built with the latest MSVC toolchain
- **Monolithic build** — single `zane.exe`, no DLL dependencies
- **Builtin HTTP server** — `Zane.serve({ fetch, port })` with hand-written HTTP/1.1 parser
- **Custom HTTP parser** (libs/http) — no llhttp, no code generation, pure C++ state machine
- **Temporal enabled** — ready for next-generation JS date/time APIs
- **Trantor networking** — async I/O via epoll/IOCP-based event loop
- **Node.js behavioral compatibility** — `node:*` modules (fs, path, os, buffer, events, stream, zlib, process) with matching API surface, not source fork
- **Zane Context** — optimized default environment with essential APIs (`console`, `Zane.*`)

## Project Structure

```
src/
├── main.cpp                    # Entry point
├── module/
│   ├── builtin/                # Zane builtins (Zane.* namespace)
│   │   └── server/             # Zane.serve() — Request, Response, Server
│   ├── node/                   # Node.js-compatible modules (node:*)
│   ├── timer.cpp               # setTimeout/setInterval
│   └── console.cpp             # console.log/debug/error
├── temporal_shims.cpp           # V8 Temporal API stubs
├── utility.hpp                  # LIFETIME_BOUND, ScopeGuard, NotNull
libs/
├── http/                        # Zane-HTTPParser (git submodule)
└── deps/                        # Third-party (zlib, brotli, zstd, trantor)
tools/
├── build.ps1                    # Incremental build script
└── check_style.py               # C++ coding style checker
docs/
└── SERVER_MODULE.md             # Builtin server architecture
```

## Prerequisites

- **Visual Studio 2022/2026** with "Desktop development with C++" workload
- **Python 3.x** (for V8 shim extraction)
- **V8 Artifacts** from [v8-zane](https://github.com/Zane-JS/v8-zane)

## Building

Open **Developer PowerShell for Visual Studio** and run:

```powershell
.\build.ps1
```

The resulting `zane.exe` will be created in the root directory.

## Running

```powershell
.\zane.exe main.js
```

## Example: HTTP Server

```js
Zane.serve({
  port: 3000,
  fetch(req, res) {
    console.log(req.method, req.url);
    res.send("Hello from Zane!");
  }
});
```

```powershell
.\zane.exe server.js
curl http://localhost:3000/
```

---

**Zane: V8 — A Gift from Google.**
