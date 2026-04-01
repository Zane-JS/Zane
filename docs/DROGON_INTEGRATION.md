# Drogon Integration Plan for Z8

## Mục tiêu
Tích hợp Drogon vào Z8 để có HTTP implementation tuân thủ chuẩn Web, xử lý tốt các edge cases.

## Lý do chọn Drogon
1. **Tuân thủ chuẩn HTTP**: Xử lý đầy đủ chunked encoding, header parsing, keep-alive
2. **Production-ready**: Đã được test kỹ trong nhiều dự án thực tế
3. **Performance cao**: Sử dụng IOCP trên Windows
4. **Tương thích tốt**: Dễ dàng tương thích với Express/Fastify patterns

## Chiến lược tích hợp

### Option 1: Build Drogon đầy đủ (Khuyến nghị cho production)
**Ưu điểm:**
- Có đầy đủ tính năng
- Được maintain bởi community
- Bug fixes tự động khi update

**Nhược điểm:**
- Build time lâu (30-60 phút lần đầu)
- Dependencies nhiều (jsoncpp, trantor, zlib, brotli, openssl)
- Binary size lớn hơn

**Các bước:**
1. Build Drogon với CMake
2. Link static library vào Z8
3. Wrap Drogon API thành node:http API

### Option 2: Cherry-pick components (Tạm thời)
**Ưu điểm:**
- Build nhanh
- Binary nhỏ
- Chỉ lấy những gì cần

**Nhược điểm:**
- Phải maintain manually
- Khó update khi Drogon có thay đổi
- Có thể miss một số edge cases

**Components cần thiết:**
- `HttpRequestParser.cc/h` - Parse HTTP requests
- `HttpResponseImpl.cc/h` - Build HTTP responses  
- `HttpServer.cc/h` - Server core
- `HttpUtils.cc/h` - Utilities
- Trantor (event loop) - Cần build riêng

### Option 3: Hybrid approach (Khuyến nghị cho development)
**Sử dụng:**
- Drogon's HTTP parser (cherry-pick)
- Z8's own IOCP implementation (đã có)
- Minimal dependencies

**Lợi ích:**
- Có HTTP parser chuẩn
- Giữ control về networking
- Build nhanh, binary nhỏ

## Implementation Plan

### Phase 1: Setup Drogon build (1-2 giờ)
```powershell
# Run build script
.\build_drogon.ps1
```

### Phase 2: Create wrapper (2-3 giờ)
- Wrap Drogon HttpServer thành node:http compatible API
- Map callbacks từ V8 sang Drogon handlers
- Handle request/response lifecycle

### Phase 3: Testing (1-2 giờ)
- Test với các HTTP clients khác nhau
- Test edge cases (chunked, keep-alive, large headers)
- Benchmark performance

## Dependencies cần thiết

### Bắt buộc:
- CMake 3.15+
- Visual Studio 2019+ (MSVC)
- OpenSSL (đã có trong deps)
- zlib (đã có)
- brotli (đã có)

### Tùy chọn:
- jsoncpp (cho JSON config - có thể skip)
- yaml-cpp (cho YAML config - có thể skip)
- PostgreSQL/MySQL libs (cho ORM - không cần)
- Redis libs (không cần)

## Estimated Timeline
- **Full Drogon build**: 4-6 giờ (bao gồm troubleshooting)
- **Cherry-pick approach**: 2-3 giờ
- **Hybrid approach**: 3-4 giờ

## Recommendation
Bắt đầu với **Hybrid approach**:
1. Sử dụng HTTP parser của Drogon
2. Giữ IOCP implementation hiện tại của Z8
3. Sau khi stable, có thể migrate sang full Drogon

## Next Steps
1. Chạy `build_drogon.ps1` để build Drogon
2. Hoặc cherry-pick HTTP parser components
3. Update `http.cpp` để sử dụng Drogon parser
