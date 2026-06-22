# Hướng Dẫn Sử Dụng Công Cụ Kiểm Tra Coding Style

## 🎯 Mục Đích

Các công cụ này giúp đảm bảo code của Zane luôn tuân thủ coding standards mà không cần refactor lại nhiều lần.

## 📦 Cài Đặt Lần Đầu

### Bước 1: Cài đặt Git Hooks

```bash
cd Zane-app
python tools/install_hooks.py
```

Kết quả:

```
✅ Git hooks installed successfully!
   Pre-commit hook: D:\Zane\Zane-app\.git\hooks\pre-commit

📝 The pre-commit hook will now check coding style before each commit.
   To skip the check, use: git commit --no-verify
```

### Bước 2: Kiểm tra code hiện tại

```bash
python tools/check_style.py
```

Nếu có lỗi, sửa chúng trước khi tiếp tục.

---

## 🔍 Sử Dụng Hàng Ngày

### 1. Kiểm tra file đang làm việc

```bash
# Kiểm tra một file cụ thể
python tools/check_style.py src/module/myfile.cpp

# Kiểm tra một thư mục
python tools/check_style.py src/module/
```

### 2. Commit code

Khi bạn commit, Git hook sẽ tự động kiểm tra:

```bash
git add .
git commit -m "Your message"
```

**Nếu có lỗi:**

```
❌ Commit rejected due to coding style violations.
💡 Fix the errors above or use 'git commit --no-verify' to skip this check.
```

**Nếu pass:**

```
✅ Coding style check passed!
[main abc1234] Your message
```

### 3. Bỏ qua kiểm tra (khi cần thiết)

```bash
git commit --no-verify -m "Emergency fix"
```

⚠️ **Chỉ dùng khi thực sự cần thiết!**

---

## 📋 Các Quy Tắc Được Kiểm Tra

### ✅ Naming Conventions

| Loại               | Quy tắc      | Ví dụ Đúng   | Ví dụ Sai    |
| ------------------ | ------------ | ------------ | ------------ |
| Methods            | camelCase    | `readFile()` | `ReadFile()` |
| Member variables   | `m_` prefix  | `m_count`    | `count`      |
| Pointer parameters | `p_` prefix  | `p_isolate`  | `isolate`    |
| Static variables   | `s_` prefix  | `s_instance` | `instance`   |
| Shared pointers    | `sp_` prefix | `sp_task`    | `task`       |
| Unique pointers    | `up_` prefix | `up_timer`   | `timer`      |

### ✅ Type System

- ✅ Dùng `int32_t` thay vì `int`
- ✅ Dùng `uint32_t` thay vì `unsigned int`
- ✅ Rõ ràng về kích thước integer

---

## 🎨 Ví Dụ

### ❌ SAI:

```cpp
class MyClass {
    int count;  // ❌ Thiếu m_ prefix, dùng int

    void ReadFile(const v8::FunctionCallbackInfo<v8::Value>& args) {  // ❌ PascalCase
        v8::Isolate* isolate = args.GetIsolate();  // ❌ Thiếu p_ prefix
    }
};
```

### ✅ ĐÚNG:

```cpp
class MyClass {
    int32_t m_count;  // ✅ Có m_ prefix, dùng int32_t

    void readFile(const v8::FunctionCallbackInfo<v8::Value>& args) {  // ✅ camelCase
        v8::Isolate* p_isolate = args.GetIsolate();  // ✅ Có p_ prefix
    }
};
```

---

## 🔧 Xử Lý Lỗi Thường Gặp

### Lỗi: "Method should use camelCase"

**Nguyên nhân:** Method dùng PascalCase

**Cách sửa:**

```cpp
// ❌ SAI
void FS::ReadFile(...) { }

// ✅ ĐÚNG
void FS::readFile(...) { }
```

### Warning: "Use 'int32_t' instead of 'int'"

**Nguyên nhân:** Dùng `int` thay vì `int32_t`

**Cách sửa:**

```cpp
// ❌ SAI
int count = 0;

// ✅ ĐÚNG
int32_t count = 0;
```

### Warning: "Member variable should use m\_ prefix"

**Nguyên nhân:** Member variable thiếu prefix

**Cách sửa:**

```cpp
class MyClass {
    // ❌ SAI
    int32_t count;

    // ✅ ĐÚNG
    int32_t m_count;
};
```

---

## 💡 Tips & Best Practices

### 1. Chạy checker thường xuyên

```bash
# Thêm alias vào shell của bạn
alias check-style="python tools/check_style.py"

# Sử dụng
check-style src/module/myfile.cpp
```

### 2. Kiểm tra trước khi commit lớn

```bash
# Kiểm tra tất cả thay đổi
python tools/check_style.py src/

# Nếu OK, commit
git add .
git commit -m "Big refactor"
```

### 3. Tích hợp vào IDE

**VS Code:** Thêm vào tasks.json

```json
{
  "label": "Check Style",
  "type": "shell",
  "command": "python tools/check_style.py ${file}",
  "problemMatcher": []
}
```

### 4. CI/CD Integration

Thêm vào GitHub Actions:

```yaml
- name: Check Coding Style
  run: python tools/check_style.py
  working-directory: ./Zane-app
```

---

## 🐛 Troubleshooting

### Hook không chạy?

1. Kiểm tra file tồn tại:

   ```bash
   ls -la .git/hooks/pre-commit
   ```

2. Cài đặt lại:
   ```bash
   python tools/install_hooks.py
   ```

### Quá nhiều warnings?

- Tập trung vào **Errors** (❌) trước
- **Warnings** (⚠️) có thể sửa sau
- Chạy từng file một để dễ quản lý

### False positives?

Công cụ dùng heuristics nên có thể có false positives. Nếu gặp:

1. Kiểm tra thủ công
2. Báo cáo để cải thiện tool
3. Dùng `--no-verify` nếu chắc chắn code đúng

---

## 📚 Tài Liệu Tham Khảo

- [CODING_STYLE_vi.md](../docs/CODING_STYLE_vi.md) - Coding standards đầy đủ
- [tools/README.md](README.md) - Tài liệu công cụ chi tiết

---

## ❓ FAQ

**Q: Có bắt buộc phải dùng hook không?**  
A: Không bắt buộc, nhưng **rất khuyến khích** để tránh refactor lại nhiều lần.

**Q: Làm sao để tắt hook tạm thời?**  
A: Dùng `git commit --no-verify`

**Q: Tool có chậm không?**  
A: Không, chỉ kiểm tra files đã thay đổi, rất nhanh.

**Q: Có thể customize rules không?**  
A: Có, edit file `tools/check_style.py` và test kỹ.

---

**Cập nhật:** 2026-01-29  
**Người duy trì:** Zane Team
