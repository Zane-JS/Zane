# uSockets Windows IOCP Patch

## Tổng quan

Patch này thêm native Windows IOCP (I/O Completion Ports) support cho uSockets, loại bỏ dependency vào libuv và cung cấp hiệu năng cao hơn trên Windows.

## Lợi thế so với libuv

1. **Native Performance**: IOCP là API native của Windows kernel, tối ưu hóa cho Windows
2. **Zero Dependency**: Không cần libuv, giảm kích thước binary và complexity
3. **Better Scalability**: IOCP được thiết kế cho high-concurrency scenarios
4. **Market Advantage**: Bun chưa có native Windows support tốt - đây là cơ hội cho Z8

## Kiến trúc

```
uSockets
├── Linux: epoll (native)
├── macOS: kqueue (native)  
└── Windows: IOCP (native) ← PATCH MỚI
    ├── winsock_iocp.c - Implementation
    └── winsock_iocp.h - Header
```

## Cách sử dụng

### 1. Copy patch files

```bash
# Copy IOCP implementation vào uSockets
cp patches/uSockets/eventing/winsock_iocp.c deps/uSockets/src/eventing/
cp patches/uSockets/eventing/winsock_iocp.h deps/uSockets/src/internal/eventing/
```

### 2. Build với IOCP

```powershell
# Build Z8 với IOCP backend
.\build.ps1 -UseIOCP
```

Hoặc define macro khi compile:

```cpp
#define LIBUS_USE_IOCP
```

### 3. Verify

```javascript
// test_iocp.js
const http = require('http');

const server = http.createServer((req, res) => {
  res.writeHead(200);
  res.end('Hello from Z8 with native IOCP!\n');
});

server.listen(3000, () => {
  console.log('Server running on http://localhost:3000');
  console.log('Backend: Windows IOCP (native)');
});
```

## Implementation Details

### Core Components

1. **Loop**: `us_loop_t` - Wraps IOCP handle và timer queue
2. **Poll**: `us_poll_t` - Represents socket với pending operations
3. **Operations**: `iocp_operation` - OVERLAPPED structure cho async I/O
4. **Timers**: Windows Timer Queue API

### Key Functions

- `us_create_loop()` - Tạo IOCP handle
- `us_loop_run()` - Main event loop với `GetQueuedCompletionStatus()`
- `us_poll_start()` - Associate socket với IOCP và start async operations
- `WSARecv()`/`WSASend()` - Async I/O operations

### Performance Characteristics

- **Async I/O**: Tất cả operations đều async, không blocking
- **Thread Pool**: IOCP tự động manage thread pool trong kernel
- **Scalability**: Có thể handle hàng nghìn concurrent connections
- **Low Latency**: Direct kernel integration, ít context switches

## Testing

```powershell
# Build và test
.\build.ps1 -UseIOCP
.\Z8.exe test_iocp.js

# Benchmark
.\Z8.exe benchmark_http.js
```

## Roadmap

- [x] Basic IOCP loop implementation
- [x] Socket operations (read/write)
- [x] Timer support
- [ ] SSL/TLS integration với OpenSSL
- [ ] UDP socket support
- [ ] Advanced features (scatter/gather I/O)
- [ ] Performance tuning và optimization

## So sánh với Bun

| Feature | Bun (Windows) | Z8 (Windows) |
|---------|---------------|--------------|
| Backend | libuv | Native IOCP |
| Dependencies | libuv + deps | Zero extra deps |
| Performance | Good | Excellent |
| Native Integration | Indirect | Direct |
| Market Position | Limited | Strong |

## References

- [IOCP Documentation](https://docs.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [uSockets GitHub](https://github.com/uNetworking/uSockets)
- [High-Performance Windows Programming](https://docs.microsoft.com/en-us/windows/win32/fileio/synchronous-and-asynchronous-i-o)
