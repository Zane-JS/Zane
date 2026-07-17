# Zane Server Module — Kiến trúc module runtime

## 1. Phân loại module

Zane có 3 cấp module:

| Cấp | Đường dẫn | Cách truy cập | Ví dụ |
|-----|-----------|---------------|-------|
| **Builtin** | `src/module/builtin/` | Global `Zane.*` | `Zane.serve()` |
| **Namespace** | `src/module/zane/` | `import from 'zane:*'` | `zane:fs`, `zane:crypto` |
| **Node compat** | `src/module/node/` | `import from 'node:*'` | `node:fs`, `node:path` |

### Builtin (Global namespace)

Các module đặc biệt, có sẵn toàn cục qua namespace `Zane`:

- `Zane.serve()` — HTTP server
- `Zane.version` — runtime version
- Không cần import, không cần ESM resolution

### Zane modules (zane:*)

Các module tiện ích nâng cao, import qua specifier `zane:*`:

```js
import { readFile } from 'zane:fs'
```

### Node compat (node:*)

Tương thích với Node.js API hiện có, implement sau khi builtin module hoàn thiện.

---

## 2. Builtin Server Module

### 2.1 Directory Structure

```
src/module/builtin/
├── builtin.hpp              // Khai báo builtin namespace, global template setup
├── builtin.cpp              // Đăng ký Zane global object vào runtime
├── server/
│   ├── server.hpp           // Zane.serve() — C++ interface
│   ├── server.cpp           // Implementation (Trantor event loop + TCP)
│   ├── request.hpp          // Request object (C++ backed)
│   ├── request.cpp
│   ├── response.hpp         // Response object (C++ backed)
│   └── response.cpp
```

### 2.2 API JavaScript

```js
// ==========================================
// Zane.serve() — Native HTTP server
// ==========================================
const server = Zane.serve({
    port: 8080,                     // Mặc định 0 (random port)
    hostname: '0.0.0.0',           // Mặc định '0.0.0.0'
    idleTimeout: 30,                // Timeout keep-alive (giây)
    maxRequestBodySize: 1024 * 1024, // 1MB

    // fetch handler — điểm chạm duy nhất giữa C++ và JS
    async fetch(req) {
        const url = new URL(req.url);

        // Text response
        if (url.pathname === '/') {
            return new Response('Hello', {
                headers: { 'Content-Type': 'text/plain' }
            });
        }

        // JSON response
        if (url.pathname === '/api/data' && req.method === 'POST') {
            const payload = await req.json();
            return Response.json({ ok: true, data: payload });
        }

        // Stream response
        if (url.pathname === '/stream') {
            const stream = new ReadableStream({
                start(controller) {
                    controller.enqueue('chunk 1\n');
                    controller.enqueue('chunk 2\n');
                    controller.close();
                }
            });
            return new Response(stream, {
                headers: { 'Content-Type': 'text/plain' }
            });
        }

        return new Response('Not Found', { status: 404 });
    },

    // error handler — xử lý lỗi tập trung
    // Tránh crash Trantor event loop
    error(err) {
        console.error('Server error:', err);
        return new Response('Internal Server Error', { status: 500 });
    }
});

// server.close() — graceful shutdown
server.close(() => {
    console.log('Server closed');
});
```

### 2.3 C++ Classes

```
┌──────────────────────────┐
│     Zane::serve()        │  ← Global function, entry point
│  ZaneServer::serve()     │     Singleton manager
├──────────────────────────┤
│       ZaneServer         │  ← Quản lý Trantor TcpServer
│  - m_tcp_server          │     Event loop, accept connections
│  - m_fetch_handler       │     JS fetch callback (Global)
│  - m_error_handler       │     JS error callback (Global)
│  - m_is_closing          │
├──────────────────────────┤
│      ZaneRequest         │  ← Mỗi request một instance
│  - m_method              │     Method GET/POST
│  - m_path                │     Pathname /api/data
│  - m_headers             │     Headers map
│  - m_body                │     Buffer body
│  - m_connection          │     Back-ref đến TCP connection
│  - json()                │     Method: parse body → JS object
│  - text()                │     Method: body → JS string
├──────────────────────────┤
│      ZaneResponse        │  ← Gắn với một request duy nhất
│  - m_connection          │     TCP connection ref
│  - m_status              │     Status code
│  - m_headers             │     Headers map
│  - m_has_ended           │     Flag prevent double-end
│  - send()                │     Send string body
│  - json()                │     Send JSON body
│  - end()                 │     Finalize response
│  - sendStream()          │     Pipe ReadableStream → TCP
└──────────────────────────┘
```

### 2.4 Data Flow

```
[JS] Zane.serve({ fetch, error })
              │
              ▼
[C++] ZaneServer::serve()
              │
              ▼
      Trantor TcpServer::start()
              │
              ▼
    ┌─── accept() ←───┐
    │    onRecv()      │  ← Trantor callback
    │    parse_llhttp  │
    │    ↓             │
    │  ZaneRequest     │
    │    ↓             │
    │  Call fetch(req) │  ← V8 Function::Call
    │    ↓             │
    │  await response  │  ← JS returns Response
    │    ↓             │
    │  ZaneResponse    │
    │    ↓             │
    │  TCP send()      │
    └──────────────────┘
```

### 2.5 Memory & Lifetime

| Object | Tạo | Hủy | Ref |
|--------|-----|-----|-----|
| `ZaneServer` | `Zane.serve()` | `server.close()` | Global handle |
| `ZaneRequest` | Mỗi request mới | Sau khi response end | Local handle (V8) |
| `ZaneResponse` | Song song Request | Sau khi TCP send xong | Local handle (V8) |
| `JS fetch callback` | Khi serve() | Khi close() | `v8::Global` trong ZaneServer |
| `JS error callback` | Khi serve() | Khi close() | `v8::Global` trong ZaneServer |

Quan trọng:
- Request/Response là C++ object, nhưng lifetime gắn với V8 `v8::Local` handle
- Khi response kết thúc, V8 GC có thể thu hồi cả Request và Response
- `v8::Global` cho fetch/error handler được free thủ công trong `~ZaneServer()`

### 2.6 Request API (C++)

```cpp
namespace zane::builtin {

class ZaneRequest {
public:
    // Accessors — gọi từ V8 callback
    auto method()   const -> const std::string&;
    auto url()      const -> const std::string&;
    auto pathname() const -> const std::string&;
    auto headers()  const -> const std::map<std::string, std::string>&;

    // Body access (awaited from JS)
    auto body()     const -> const std::vector<uint8_t>&;

    // V8 helpers
    auto json(v8::Isolate*)    -> v8::Local<v8::Value>;
    auto text(v8::Isolate*)    -> v8::Local<v8::String>;
    auto formData(v8::Isolate*) -> v8::Local<v8::Value>;

private:
    std::string m_method;
    std::string m_path;
    std::string m_pathname;       // Parsed từ m_path (trước ?)
    std::map<std::string, std::string> m_headers;
    std::vector<uint8_t> m_body;
    TrantorConnection* m_connection;  // Raw ptr, lifetime managed by server
};

} // namespace zane::builtin
```

### 2.7 Response API (C++)

```cpp
namespace zane::builtin {

class ZaneResponse {
public:
    // Set status
    void setStatus(int32_t code);
    void setHeader(const std::string& name, const std::string& value);

    // Send methods
    void send(v8::Isolate*, const std::string& body);
    void sendJson(v8::Isolate*, v8::Local<v8::Value> obj);
    void sendStream(v8::Isolate*, v8::Local<v8::Object> readable);
    void end();

    // Flags
    auto hasEnded() const -> bool;

private:
    TrantorConnection* m_connection;
    int32_t m_status = 200;
    std::map<std::string, std::string> m_headers;
    bool m_has_ended = false;
    bool m_headers_sent = false;
};

} // namespace zane::builtin
```

---

## 3. Triển khai (Implementation Plan)

### Phase 1: Foundation
- [ ] Tạo `src/module/builtin/server/request.hpp/.cpp`
- [ ] Tạo `src/module/builtin/server/response.hpp/.cpp`
- [ ] Tạo `src/module/builtin/server/server.hpp/.cpp`
- [ ] Tạo `src/module/builtin/builtin.hpp/.cpp` (global `Zane` object)

### Phase 2: Core
- [ ] `ZaneRequest` — parse HTTP request từ Trantor buffer
- [ ] `ZaneResponse` — send response qua Trantor TCP connection
- [ ] `ZaneServer` — `Zane.serve()` Trantor TcpServer wrapper

### Phase 3: V8 Integration
- [ ] Đăng ký `Zane` global object template trong `builtin.cpp`
- [ ] Gắn `Zane.serve()` function template
- [ ] Tạo Request/Response prototype objects

### Phase 4: Integration
- [ ] Gọi `ZaneServer::hasActiveServers()` trong event loop condition
- [ ] Cleanup handles khi server close
- [ ] Graceful shutdown

---

## 4. Quy tắc code mới

### Tên class
- `ZaneServer`, `ZaneRequest`, `ZaneResponse` — prefix `Zane` để phân biệt với Node compat layer sau này.

### File naming
- Tất cả chữ thường, snake_case: `server.hpp`, `request.cpp`

### V8 handle naming
- `p_isolate`, `p_context` — theo convention hiện tại
- `m_fetch_handler` — `v8::Global<v8::Function>` member

### Memory
- `v8::Global` trong class → destructor gọi `.Reset()`
- Tránh tạo `v8::Global` tạm trong hot path
