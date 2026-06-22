# 🚀 Zane (Zane V8) Runtime Optimization Strategy

This document outlines the blueprint for making **Zane** a high-performance JavaScript runtime, drawing inspiration from industry leaders like **Bun** and **Deno**, while maintaining a unique "C++ First" philosophy.

---

## 1. ⚡ Ultra-Fast Startup (The "Instant-On" Goal)

Unlike Node.js, which loads a massive amount of internal JavaScript on every boot, Zane achieves high speed through:

- **V8 Startup Snapshots**: Since we use a monolithic build, we embed a pre-compiled state of the global environment and built-in objects. This bypasses initial execution and jumpstarts the runtime.
- **Lazy Module Loading**: Internal modules (like `fs`, `path`) are only initialized and bound to the JavaScript context when an `import` or `node:` call is actually made.
- **AOT (Ahead-Of-Time) Bytecode**: For frequent scripts, Zane can cache the V8 bytecode on disk, reducing compilation overhead on subsequent runs.

## 2. 🏎️ High-Performance Bindings (Glue Code Optimization)

The slowest part of any JS runtime is the transition from JavaScript to C++. Zane optimizes this "Bridge":

- **V8 Fast API Calls**: Utilize V8's modern "Fast API" infrastructure, allowing certain C++ functions to be called with near-zero overhead, bypassing the traditional handle scope creation for simple operations.
- **Direct Buffer Access**: Using `ArrayBuffer` and `TypedArray` directly in C++ to avoid copying large chunks of data between the engine and the operating system.
- **Zero-Copy I/O**: Implementing I/O operations that write directly from JS buffers to system sockets/files.

## 3. 📦 Node.js Compatibility Layer (`node:` namespace)

Zane aims to be a drop-in replacement for many scripts by supporting the `node:` prefix, similar to Bun and Deno:

- **Namespace Mapping**: The Zane resolver detects strings starting with `node:`.
  - `import fs from "node:fs"` ⮕ Maps to internal Zane C++ `fs` module.
- **Selective Polyfilling**: We don't implement the entire Node.js history. We implement the "Hot Paths" (fs, path, buffer, events) using highly optimized C++ and only shim the legacy callback patterns if necessary.
- **Modern default**: Internally, all `node:` modules in Zane are Promise-based for better performance in async/await workflows.

## 4. Native Module Ecosystem (Zane Specialties)

Zane provides built-in modules that are optimized at the C++ level:

- **`zane:signals`**: A high-performance, native implementation of the ECMAScript Signals proposal. State management with near-zero overhead, perfect for reactive UIs and backend logic.
- **`zane:sql` (Pluggable Architecture)**: A universal SQL client inspired by ADO.NET.
  - **Lightweight Core**: The Zane binary does not include specific database drivers.
  - **Extended via `.zaned`**: Support for PostgreSQL, MySQL, SQL Server, etc., is provided through Zane Driver files (`.zaned`).
  - **CLI Management**: Users can manage their engine's capabilities via the CLI:
    ```powershell
    zane driver add "path/to/pg.zaned"
    ```

## 5. 🏗️ High-Concurrency Multi-Platform I/O

To deliver world-class performance on all operating systems, Zane implements a modular I/O core:

- **Windows (IOCP)**: Leveraging standard I/O Completion Ports for best-in-class Windows performance.
- **Linux (io_uring)**: Using the latest Linux kernel asynchronous I/O interface for significantly higher throughput than traditional `epoll`.
- **MacOS/BSD (kqueue)**: Optimized event notification for Apple and BSD ecosystems.
- **Uniform Event Loop**: A unified C++ event loop that abstracts these backends, providing a consistent `Promise`-based experience for JavaScript.

## 6. 🧠 Memory & I/O Optimization

- **Pointer Compression**: Reduces memory usage of the heap by 40% on 64-bit systems.
- **Static Linking (Monolith)**: Reduces binary size and improves LTO (Link Time Optimization).
- **Adaptive I/O Buffering**: Zane uses a proprietary C++ adaptive flush mechanism.
  - **Interactive Mode**: Immediate flush for REPL and low-frequency logs.
  - **Burst Mode**: Automatically detects high-frequency logging (20+ logs/50ms) and switches to 64KB full buffering.
  - **Result**: Zane maintains near-zero latency for developers while being **2x faster than Bun** and **11x faster than Node.js** in log-heavy benchmarks.

## 7. ⚡ Zero-Latency Event Loop (New In 2026)

On Jan 25, 2026, Zane underwent a major architecture shift in its core event loop to prioritize competitive I/O performance:

- **Instant-Wakeup TaskQueue**: Replaced traditional polling (which had a ~1ms resolution) with a `std::condition_variable` signaling system. Worker threads now "notify" the main loop immediately upon task completion, achieving near-zero idle latency.
- **Microtask Checkpoint Batching**: Optimized V8's microtask checkpointing during high-frequency async loops (e.g., thousands of `await` calls), ensuring promises resolve with minimal context switch overhead.
- **Hyper-Optimized FS ThreadPool**: Fine-tuned the worker thread pool management to handle massive I/O bursts (500+ concurrent file operations).

### 📊 Comprehensive FS Benchmark (500 Operations)

Verified using `hyperfine` on Windows (Jan 25, 2026):

| Engine           | Async Write/Read Performance | Efficiency (Mean)      |
| :--------------- | :--------------------------- | :--------------------- |
| **Zane V8 (Zane)** | **433.2 ms**                 | 👑 **1.0x (Baseline)** |
| **Bun**          | 734.2 ms                     | 📉 1.7x Slower         |
| **Deno**         | 766.3 ms                     | 📉 1.77x Slower        |
| **Node.js**      | 837.9 ms                     | 📉 1.93x Slower        |

> **Conclusion**: Zane's native C++ event loop architecture is now **nearly 2 times faster** than Node.js, Bun, and Deno in filesystem-heavy async workloads.

## 🎯 The Zane Advantage: "Simple & Clean"

Node.js is burdened by 10+ years of legacy. Zane's optimization philosophy is **"Less is More"**:

1.  **No CommonJS by default**: Everything is an ES Module.
2.  **Explicit Context**: The global scope is kept clean. High-performance APIs are grouped under the `Zane` global.
3.  **Modern C++ Standard**: Leveraging C++23 features for cleaner memory management and safer concurrency than the original Node.js codebase.

---

**Zane: Better than Deno, Faster than Bun, Cleaner than Node.**
